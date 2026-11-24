using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Threading;

namespace Relay.Net
{
    public enum UdpState
    {
        /// <summary>UDP를 쓰지 않는다(요청하지 않았거나 아직 WELCOME 전).</summary>
        Off,
        /// <summary>UDP_BIND를 보내며 UDP_BOUND를 기다린다.</summary>
        Binding,
        /// <summary>묶였다. GAME_IN은 UDP로, 비신뢰 GAME_OUT은 UDP로 온다.</summary>
        Bound,
        /// <summary>2초 안에 묶이지 않아(또는 소켓 오류로) 포기했다. TCP만 쓴다.</summary>
        Failed,
    }

    /// <summary>
    /// TCP 연결 하나에 딸린 UDP 소켓(protocol.md 7장). 서버의 같은 주소 · 같은 포트로 connect 해 두어
    /// 운영체제가 다른 출처의 데이터그램을 걸러 내게 한다. 백그라운드 스레드 하나가 묶기(0.2초마다 UDP_BIND,
    /// 2초에 포기)와 받기를 함께 한다. 받은 GAME_OUT은 onFrame으로 넘겨 TCP와 같은 큐에 합친다.
    /// </summary>
    internal sealed class UdpChannel
    {
        private readonly Socket _socket;
        private readonly uint _token;
        private readonly Action<Frame> _onFrame;
        private readonly Action<UdpState, string> _onState;
        private Thread _thread;
        private volatile UdpState _state = UdpState.Binding;
        private volatile bool _closing;
        private int _dropped;

        public UdpState State => _state;
        /// <summary>형식이 어긋나 버린 데이터그램 수.</summary>
        public int Dropped => Volatile.Read(ref _dropped);

        public UdpChannel(IPAddress address, int port, uint token, Action<Frame> onFrame, Action<UdpState, string> onState)
        {
            _token = token;
            _onFrame = onFrame;
            _onState = onState;
            _socket = new Socket(address.AddressFamily, SocketType.Dgram, ProtocolType.Udp);
            _socket.Connect(new IPEndPoint(address, port));
        }

        public void Start()
        {
            _thread = new Thread(Run) { IsBackground = true, Name = "RelayConnection.Udp" };
            _thread.Start();
        }

        /// <summary>묶인 뒤에만 보낸다. 완성된 프레임(헤더 포함)에 토큰을 붙여 데이터그램 하나로.</summary>
        public bool Send(byte[] frame)
        {
            if (_state != UdpState.Bound)
                return false;
            return SendRaw(UdpDatagram.Build(_token, frame));
        }

        public void Close()
        {
            _closing = true;
            try
            {
                _socket.Close(); // 막혀 있는 Poll · Receive를 풀어 스레드가 끝나게 한다
            }
            catch (Exception)
            {
                // 이미 닫힌 소켓: 목적(닫힘)은 이뤄졌다.
            }
            if (_thread != null && _thread != Thread.CurrentThread)
                _thread.Join(1000);
        }

        private bool SendRaw(byte[] datagram)
        {
            try
            {
                return _socket.Send(datagram) == datagram.Length;
            }
            catch (Exception ex) when (ex is SocketException || ex is ObjectDisposedException)
            {
                // UDP 송신 실패는 그 데이터그램 하나를 잃은 것과 같다. 다음 입력이 묶음으로 메운다.
                return false;
            }
        }

        private void SetState(UdpState s, string reason)
        {
            _state = s;
            _onState(s, reason);
        }

        private void Run()
        {
            // 최대 길이보다 1바이트 크게: 넘는 데이터그램은 잘려 들어와 길이 검사에서 걸린다(Windows는 MessageSize 예외).
            var buf = new byte[UdpDatagram.MaxDatagram + 1];
            byte[] bind = UdpDatagram.Bind(_token);
            var clock = Stopwatch.StartNew();
            long nextBind = 0;
            try
            {
                while (!_closing)
                {
                    int waitMs = 100; // 묶인 뒤에는 닫힘을 확인할 주기만 있으면 된다
                    if (_state == UdpState.Binding)
                    {
                        long now = clock.ElapsedMilliseconds;
                        if (now >= UdpDatagram.BindGiveUpMs)
                        {
                            SetState(UdpState.Failed, $"no UDP_BOUND in {UdpDatagram.BindGiveUpMs} ms");
                            continue;
                        }
                        if (now >= nextBind)
                        {
                            SendRaw(bind);
                            nextBind += UdpDatagram.BindRetryMs;
                        }
                        waitMs = (int)Math.Max(1, Math.Min(nextBind, UdpDatagram.BindGiveUpMs) - clock.ElapsedMilliseconds);
                    }
                    if (!_socket.Poll(waitMs * 1000, SelectMode.SelectRead))
                        continue;
                    Receive(buf);
                }
            }
            catch (Exception ex) when (ex is SocketException || ex is ObjectDisposedException)
            {
                if (!_closing && _state != UdpState.Failed)
                    SetState(UdpState.Failed, "UDP socket error: " + ex.Message);
            }
        }

        private void Receive(byte[] buf)
        {
            int n;
            try
            {
                n = _socket.Receive(buf);
            }
            catch (SocketException ex) when (ex.SocketErrorCode == SocketError.ConnectionReset)
            {
                // Windows는 앞서 보낸 데이터그램이 ICMP "포트 닫힘"을 받으면 다음 Receive에서 이 예외를 낸다.
                // 서버가 UDP를 듣지 않는다는 뜻일 뿐이라 묶기는 시간 제한으로 포기하게 둔다.
                return;
            }
            catch (SocketException ex) when (ex.SocketErrorCode == SocketError.MessageSize)
            {
                Interlocked.Increment(ref _dropped);
                return;
            }
            if (!UdpDatagram.TryParseServer(buf, n, out Frame f))
            {
                Interlocked.Increment(ref _dropped);
                return;
            }
            if (f.Type == MsgType.UdpBound)
            {
                // 다시 보낸 UDP_BIND마다 답이 오므로 두 번째부터는 무시한다. 포기한 뒤 늦게 온 답도 받지 않는다(7장 1번).
                if (_state == UdpState.Binding)
                    SetState(UdpState.Bound, null);
                return;
            }
            // 포기한 뒤에도 GAME_OUT은 받는다: UDP_BOUND만 사라졌다면 서버는 이미 묶었다고 보고 STATE를 UDP로 보낸다.
            _onFrame(f);
        }
    }
}
