using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net.Sockets;
using System.Threading;

namespace Relay.Net
{
    public enum RelayEventKind
    {
        Connected,
        Frame,
        Closed,
    }

    /// <summary>수신 스레드가 메인 스레드에 넘기는 일 하나. Closed면 Reason에 까닭이 있다.</summary>
    public readonly struct RelayEvent
    {
        public readonly RelayEventKind Kind;
        public readonly Frame Frame;
        public readonly string Reason;

        public RelayEvent(RelayEventKind kind, Frame frame, string reason)
        {
            Kind = kind;
            Frame = frame;
            Reason = reason;
        }
    }

    /// <summary>
    /// 서버와의 TCP 연결 하나. 접속과 수신은 백그라운드 스레드가 하고, 받은 프레임은 스레드 안전 큐에 쌓는다.
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

        public bool IsConnected => _connected;

        /// <summary>접속을 시작한다. 결과는 큐에 Connected 또는 Closed로 온다. 한 객체에 한 번만 부른다.</summary>
        public void Start(string host, int port, int connectTimeoutMs = 5000)
        {
            if (_thread != null)
                throw new InvalidOperationException("already started");
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
                Action<Frame> push = f => _inbox.Enqueue(new RelayEvent(RelayEventKind.Frame, f, null));
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
        }
    }
}
