using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading;

namespace Relay.Net
{
    public enum RelayEventKind
    {
        Connected,
        Frame,
        Closed,
        /// <summary>UDP_BOUND를 받았다(7장). 이제 GAME_IN을 UDP로 보낼 수 있다.</summary>
        UdpBound,
        /// <summary>UDP를 포기했다. Reason에 까닭. TCP는 그대로다.</summary>
        UdpFailed,
    }

    /// <summary>수신 스레드가 메인 스레드에 넘기는 일 하나. Closed · UdpFailed면 Reason에 까닭이 있다.</summary>
    public readonly struct RelayEvent
    {
        public readonly RelayEventKind Kind;
        public readonly Frame Frame;
        public readonly string Reason;
        /// <summary>이 Frame이 UDP 데이터그램으로 왔나. 순서가 바뀌어 올 수 있다는 표시이자 테스트의 확인 수단.</summary>
        public readonly bool ViaUdp;

        public RelayEvent(RelayEventKind kind, Frame frame, string reason, bool viaUdp = false)
        {
            Kind = kind;
            Frame = frame;
            Reason = reason;
            ViaUdp = viaUdp;
        }
    }

    /// <summary>
    /// 서버와의 TCP 연결 하나와, 원하면 거기 딸린 UDP(protocol.md 7장). 접속과 수신은 백그라운드 스레드가 하고,
    /// 받은 프레임은 TCP · UDP 모두 같은 스레드 안전 큐에 쌓는다.
    /// Unity API는 메인 스레드에서만 부를 수 있으므로(overview 7절) 해석 · 반응은 큐를 비우는 쪽이 한다.
    /// 송신은 어느 스레드에서 불러도 되도록 lock으로 프레임 단위 Write를 묶는다.
    /// </summary>
    public sealed class RelayConnection : IDisposable
    {
        private readonly ConcurrentQueue<RelayEvent> _inbox = new ConcurrentQueue<RelayEvent>();
        private readonly object _sendLock = new object();
        private TcpClient _client;
        private NetworkStream _stream;
        private Thread _thread;
        private volatile bool _connected;
        private volatile bool _closing;
        private int _closedSignaled;
        private bool _useUdp;
        private int _port;
        private volatile UdpChannel _udp;

        public bool IsConnected => _connected;

        /// <summary>UDP를 요청하지 않았거나 WELCOME 전이면 Off.</summary>
        public UdpState UdpState => _udp?.State ?? UdpState.Off;

        /// <summary>형식이 어긋나 버린 UDP 데이터그램 수(연결은 끊지 않는다, 7장).</summary>
        public int UdpDropped => _udp?.Dropped ?? 0;

        /// <summary>
        /// 접속을 시작한다. 결과는 큐에 Connected 또는 Closed로 온다. 한 객체에 한 번만 부른다.
        /// useUdp면 WELCOME을 받는 즉시 같은 주소 · 같은 포트의 UDP로 묶기를 시작하고, 결과는 UdpBound · UdpFailed로 온다.
        /// </summary>
        public void Start(string host, int port, int connectTimeoutMs = 5000, bool useUdp = false)
        {
            if (_thread != null)
                throw new InvalidOperationException("already started");
            _useUdp = useUdp;
            _port = port;
            _thread = new Thread(() => Run(host, port, connectTimeoutMs))
            {
                IsBackground = true, // 앱 종료를 막지 않게
                Name = "RelayConnection.Receive",
            };
            _thread.Start();
        }

        public bool TryReceive(out RelayEvent e) => _inbox.TryDequeue(out e);

        /// <summary>
        /// 완성된 프레임(헤더 포함)을 보낸다. 연결 전이거나 끊겼으면 false.
        /// 쓰기 실패는 연결이 죽었다는 뜻이므로 닫고 수신 스레드가 Closed를 알리게 한다.
        /// </summary>
        public bool Send(byte[] frame)
        {
            if (!_connected)
                return false;
            try
            {
                lock (_sendLock)
                    _stream.Write(frame, 0, frame.Length);
                return true;
            }
            catch (Exception ex) when (ex is IOException || ex is ObjectDisposedException || ex is SocketException)
            {
                Shutdown();
                return false;
            }
        }

        /// <summary>
        /// 완성된 프레임(헤더 포함)을 UDP 데이터그램 하나로 보낸다(토큰은 여기서 붙인다). 묶이지 않았으면 false:
        /// 그때는 부른 쪽이 TCP 형식으로 Send 한다. UDP 송신 실패는 연결을 닫지 않는다.
        /// </summary>
        public bool SendUdp(byte[] frame)
        {
            UdpChannel udp = _udp;
            return _connected && udp != null && udp.Send(frame);
        }

        /// <summary>연결을 닫는다. 큐에는 Closed("closed locally")가 한 번 들어간다.</summary>
        public void Close()
        {
            _closing = true;
            Shutdown();
            // 수신 스레드는 소켓이 닫혀 곧 빠져나온다. 메인 스레드를 오래 잡지 않도록 짧게만 기다린다.
            if (_thread != null && _thread != Thread.CurrentThread)
                _thread.Join(1000);
        }

        public void Dispose() => Close();

        private void Run(string host, int port, int connectTimeoutMs)
        {
            string reason = "server closed the connection";
            try
            {
                _client = new TcpClient { NoDelay = true }; // 1/30초마다 가는 작은 입력을 Nagle이 묶지 않게
                var connect = _client.ConnectAsync(host, port);
                if (!connect.Wait(connectTimeoutMs))
                    throw new TimeoutException($"connect {host}:{port} timed out");
                _stream = _client.GetStream();
                _connected = true;
                if (_closing) // 접속 중에 Close가 불렸으면 바로 정리
                    return;
                _inbox.Enqueue(new RelayEvent(RelayEventKind.Connected, default, null));

                var decoder = new FrameDecoder();
                var buf = new byte[4096];
                Action<Frame> push = f =>
                {
                    _inbox.Enqueue(new RelayEvent(RelayEventKind.Frame, f, null));
                    if (f.Type == MsgType.Welcome && _useUdp && _udp == null)
                        StartUdp(f.Payload);
                };
                while (true)
                {
                    int n = _stream.Read(buf, 0, buf.Length);
                    if (n <= 0)
                        break;
                    decoder.Push(buf, 0, n, push);
                }
            }
            catch (ProtocolException ex)
            {
                reason = "protocol error: " + ex.Message;
            }
            catch (AggregateException ex)
            {
                reason = "connect failed: " + (ex.InnerException?.Message ?? ex.Message);
            }
            catch (Exception ex)
            {
                reason = _closing ? "closed locally" : ex.GetType().Name + ": " + ex.Message;
            }
            finally
            {
                if (_closing)
                    reason = "closed locally";
                Shutdown();
                if (Interlocked.Exchange(ref _closedSignaled, 1) == 0)
                    _inbox.Enqueue(new RelayEvent(RelayEventKind.Closed, default, reason));
            }
        }

        /// <summary>
        /// WELCOME의 udp_token(conn_id 다음 u32)으로 묶기를 시작한다. 주소는 TCP가 실제로 붙은 서버 주소를 쓴다:
        /// 호스트 이름이 여러 주소로 풀려도 TCP와 같은 서버로 가야 토큰이 맞는다.
        /// </summary>
        private void StartUdp(byte[] welcome)
        {
            uint token = BigEndian.ReadU32(welcome, 4); // 길이는 FrameDecoder가 9 이상으로 이미 확인했다
            try
            {
                var remote = (IPEndPoint)_client.Client.RemoteEndPoint;
                // 이중 스택 TCP 소켓이면 IPv4 서버도 ::ffff:a.b.c.d로 보인다. UDP 소켓은 IPv6 전용으로 만들어지므로
                // 그 주소로는 IPv4 서버에 닿지 않는다: 원래 IPv4 주소로 되돌린다.
                IPAddress address = remote.Address.IsIPv4MappedToIPv6 ? remote.Address.MapToIPv4() : remote.Address;
                var udp = new UdpChannel(address, _port, token,
                    f =>
                    {
                        if (_connected) // 닫힌 뒤 늦게 온 데이터그램이 다음 연결의 화면을 흔들지 않게
                            _inbox.Enqueue(new RelayEvent(RelayEventKind.Frame, f, null, viaUdp: true));
                    },
                    (state, reason) => _inbox.Enqueue(new RelayEvent(
                        state == UdpState.Bound ? RelayEventKind.UdpBound : RelayEventKind.UdpFailed, default, reason)));
                _udp = udp;
                udp.Start();
                if (!_connected) // 시작하는 사이에 닫혔으면 Shutdown이 못 본 소켓을 여기서 닫는다
                    udp.Close();
            }
            catch (SocketException ex)
            {
                _inbox.Enqueue(new RelayEvent(RelayEventKind.UdpFailed, default, "UDP socket: " + ex.Message));
            }
        }

        private void Shutdown()
        {
            _connected = false;
            try
            {
                _client?.Close(); // 막혀 있는 Read를 풀어 수신 스레드가 끝나게 한다
            }
            catch (Exception)
            {
                // 이미 닫힌 소켓을 다시 닫는 것은 무시해도 된다: 목적(닫힘)은 이미 이뤄졌다.
            }
            // TCP가 끊기면 토큰도 사라진다(7장 묶기 5번). UDP만 남겨 둘 이유가 없다.
            _udp?.Close();
        }
    }
}
