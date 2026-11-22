using System;
using System.Globalization;
using System.IO;
using UnityEngine;

namespace Relay.Lobby
{
    /// <summary>
    /// 사람 없이 두 창 대전을 확인하기 위한 명령줄 옵션(-relayHost, -autoplay, -log 등).
    /// 옵션이 하나도 없으면 아무것도 바꾸지 않아 일반 실행에는 영향이 없다.
    /// 게임 전용 옵션(-bot, -winScore)도 같은 방식으로 읽도록 Has · Get을 열어 둔다.
    /// </summary>
    public sealed class RunOptions
    {
        private readonly string[] _args;

        public RunOptions(string[] args)
        {
            _args = args ?? Array.Empty<string>();
        }

        public static RunOptions FromCommandLine() => new RunOptions(Environment.GetCommandLineArgs());

        public bool Has(string name) => Array.FindIndex(_args, a => string.Equals(a, name, StringComparison.OrdinalIgnoreCase)) >= 0;

        public string Get(string name)
        {
            int i = Array.FindIndex(_args, a => string.Equals(a, name, StringComparison.OrdinalIgnoreCase));
            return i >= 0 && i + 1 < _args.Length ? _args[i + 1] : null;
        }

        public int? GetInt(string name) =>
            int.TryParse(Get(name), NumberStyles.Integer, CultureInfo.InvariantCulture, out int v) ? v : (int?)null;

        public float? GetFloat(string name) =>
            float.TryParse(Get(name), NumberStyles.Float, CultureInfo.InvariantCulture, out float v) ? v : (float?)null;
    }

    /// <summary>
    /// -log 경로에 시각과 함께 한 줄씩 남긴다. 두 창의 로그를 나란히 맞춰 볼 수 있게
    /// 앱 시작 뒤 초와 벽시계 시각을 함께 적는다. 경로가 없으면 Unity 로그에만 남긴다.
    /// </summary>
    public sealed class RunLog : IDisposable
    {
        private readonly StreamWriter _writer;

        public RunLog(string path)
        {
            if (string.IsNullOrEmpty(path))
                return;
            string dir = Path.GetDirectoryName(Path.GetFullPath(path));
            if (!string.IsNullOrEmpty(dir))
                Directory.CreateDirectory(dir);
            _writer = new StreamWriter(path, false) { AutoFlush = true }; // 강제 종료돼도 앞줄은 남게
        }

        public void Write(string line)
        {
            string stamped = string.Format(CultureInfo.InvariantCulture, "[{0,8:F3}s {1:HH:mm:ss.fff}] {2}",
                Time.realtimeSinceStartupAsDouble, DateTime.Now, line);
            _writer?.WriteLine(stamped);
            Debug.Log("[relay] " + stamped);
        }

        public void Dispose() => _writer?.Dispose();
    }
}
