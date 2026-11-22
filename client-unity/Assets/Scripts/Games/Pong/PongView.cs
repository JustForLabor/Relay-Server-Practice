using System;
using System.Collections.Generic;
using Pong.Protocol;
using Relay.Lobby;
using UnityEngine;
using UnityEngine.UI;

namespace Pong.Game
{
    /// <summary>
    /// 경기장 그림(흰 스프라이트 하나를 늘려 공 · 패들 · 선을 그린다)과 점수판 · 결과 화면.
    /// 월드 1단위 = 경기장 1단위. 서버 좌표는 원점이 왼쪽 위이고 y가 아래로 늘어나므로
    /// 월드 좌표 = (x − field_w/2, field_h/2 − y)로 뒤집어 경기장 가운데를 원점에 둔다.
    /// </summary>
    public sealed class PongView
    {
        private static readonly Color MyColor = new Color(1f, 0.85f, 0.25f);
        private static readonly Color OtherColor = Color.white;
        private static readonly Color LineColor = new Color(1f, 1f, 1f, 0.35f);
        private static readonly Color FieldColor = new Color(0.06f, 0.07f, 0.1f);

        private readonly Camera _cam;
        private readonly Sprite _white;
        private readonly Transform _world;
        private readonly SpriteRenderer _field, _top, _bottom, _ball, _paddle0, _paddle1;
        private readonly List<SpriteRenderer> _dashes = new List<SpriteRenderer>();
        private readonly Text _score, _role, _banner;
        private readonly RectTransform _result;
        private readonly Text _resultTitle, _resultScore, _resultVotes;
        private readonly Button _rematch;
        private PongSettings _rules = new PongSettings();
        private int _mySlot;

        public PongView(Camera cam, RectTransform uiRoot, Action onRematch, Action onLeave)
        {
            _cam = cam;
            _cam.orthographic = true;
            _cam.backgroundColor = Color.black;
            _white = MakeWhiteSprite();

            _world = new GameObject("PongField").transform;
            _field = Rect("Field", FieldColor, 0);
            _top = Rect("WallTop", LineColor, 1);
            _bottom = Rect("WallBottom", LineColor, 1);
            _paddle0 = Rect("Paddle0", OtherColor, 2);
            _paddle1 = Rect("Paddle1", OtherColor, 2);
            _ball = Rect("Ball", Color.white, 3);

            // HUD는 로비 UI 아래에 깔리도록 먼저 만든다(형제 순서가 그리는 순서).
            var hud = UiKit.Child(uiRoot, "PongHud");
            UiKit.Stretch(hud);
            hud.SetAsFirstSibling();
            _score = UiKit.Overlay(hud, "Score", 44, TextAnchor.UpperCenter, new Vector2(0, 1), new Vector2(1, 1));
            ((RectTransform)_score.transform).offsetMin = new Vector2(10, -64);
            _role = UiKit.Overlay(hud, "Role", 16, TextAnchor.UpperRight, new Vector2(0.5f, 1), new Vector2(1, 1));
            ((RectTransform)_role.transform).offsetMin = new Vector2(10, -34);
            _role.color = MyColor;
            _banner = UiKit.Overlay(hud, "Banner", 22, TextAnchor.UpperCenter, new Vector2(0, 1), new Vector2(1, 1));
            ((RectTransform)_banner.transform).offsetMin = new Vector2(10, -100);
            ((RectTransform)_banner.transform).offsetMax = new Vector2(-10, -66);

            _result = UiKit.Panel(uiRoot, "ResultPanel", 440, new Vector2(0.5f, 0.5f));
            _resultTitle = UiKit.Label(_result, "", 34, TextAnchor.MiddleCenter);
            _resultScore = UiKit.Label(_result, "", 22, TextAnchor.MiddleCenter);
            _resultVotes = UiKit.Label(_result, "", 18, TextAnchor.MiddleCenter);
            RectTransform row = UiKit.Row(_result, 40);
            _rematch = UiKit.Button(row, "다시 하기", onRematch);
            UiKit.Button(row, "나가기", onLeave);

            SetFieldVisible(false);
            HideResult();
        }

        /// <summary>ROOM_JOINED로 받은 실제 규칙에 맞춰 경기장 크기와 내 패들 색을 정한다.</summary>
        public void Setup(PongSettings rules, int mySlot)
        {
            _rules = rules;
            _mySlot = mySlot;
            float w = rules.field_w, h = rules.field_h;
            Place(_field, w / 2f, h / 2f, w, h);
            Place(_top, w / 2f, -2f, w, 4f);
            Place(_bottom, w / 2f, h + 2f, w, 4f);
            Size(_paddle0, rules.paddle_w, rules.paddle_h);
            Size(_paddle1, rules.paddle_w, rules.paddle_h);
            Size(_ball, rules.ball_size, rules.ball_size);
            _paddle0.color = mySlot == 0 ? MyColor : OtherColor;
            _paddle1.color = mySlot == 1 ? MyColor : OtherColor;
            BuildCenterLine(w, h);
            _role.text = $"나 = {(mySlot == 0 ? "왼쪽" : "오른쪽")} (노란 패들) · 첫 {rules.win_score}점 승리";
            FitCamera();
        }

        public void SetFieldVisible(bool on)
        {
            _world.gameObject.SetActive(on);
            _score.gameObject.SetActive(on);
            _role.gameObject.SetActive(on);
            _banner.gameObject.SetActive(on);
        }

        public void SetBanner(string text) => _banner.text = text;

        public void Draw(PongPose p)
        {
            Move(_paddle0, _rules.paddle_inset, p.Paddle0Y);
            Move(_paddle1, _rules.field_w - _rules.paddle_inset, p.Paddle1Y);
            Move(_ball, p.BallX, p.BallY);
            _score.text = $"{p.Score0}   :   {p.Score1}";
            FitCamera(); // 창 크기가 바뀌어도 경기장이 다 보이게(가로세로 비율 계산뿐이라 싸다)
        }

        /// <summary>규칙의 시작 상태(6.1절): 공은 가운데, 두 패들 y = field_h/2, 0 : 0.</summary>
        public void DrawInitial()
        {
            Draw(new PongPose
            {
                BallX = _rules.field_w / 2, BallY = _rules.field_h / 2,
                Paddle0Y = _rules.field_h / 2, Paddle1Y = _rules.field_h / 2,
            });
        }

        public void ShowResult(int score0, int score1)
        {
            int mine = _mySlot == 0 ? score0 : score1;
            int theirs = _mySlot == 0 ? score1 : score0;
            _resultTitle.text = mine > theirs ? "승리!" : "패배";
            _resultScore.text = $"왼쪽 {score0} : {score1} 오른쪽";
            _rematch.interactable = true;
            UiKit.SetButtonText(_rematch, "다시 하기");
            ShowVotes(new bool[2]);
            _result.gameObject.SetActive(true);
        }

        public void ShowVotes(bool[] votes)
        {
            bool me = votes[_mySlot], other = votes[1 - _mySlot];
            _resultVotes.text = $"다시 하기 동의 — 나: {(me ? "동의함" : "아직")} · 상대: {(other ? "동의함" : "아직")}";
            if (me)
            {
                _rematch.interactable = false;
                UiKit.SetButtonText(_rematch, "상대를 기다리는 중");
            }
        }

        public void HideResult() => _result.gameObject.SetActive(false);

        public bool ResultVisible => _result.gameObject.activeSelf;

        private void FitCamera()
        {
            // 위쪽 점수판 자리(약 90px)를 남기고 경기장 전체가 들어가는 가장 큰 배율.
            float halfH = _rules.field_h / 2f;
            float halfW = _rules.field_w / 2f;
            float aspect = Mathf.Max(0.1f, _cam.aspect);
            float size = Mathf.Max(halfH * 1.32f, (halfW + 10f) / aspect);
            _cam.orthographicSize = size;
            _cam.transform.position = new Vector3(0f, halfH * 0.14f, -10f);
        }

        private void BuildCenterLine(float w, float h)
        {
            foreach (var d in _dashes)
                UnityEngine.Object.Destroy(d.gameObject);
            _dashes.Clear();
            const float dash = 16f, gap = 12f;
            for (float y = gap / 2f; y + dash <= h; y += dash + gap)
            {
                SpriteRenderer r = Rect("CenterDash", LineColor, 1);
                Place(r, w / 2f, y + dash / 2f, 3f, dash);
                _dashes.Add(r);
            }
        }

        private SpriteRenderer Rect(string name, Color c, int order)
        {
            var go = new GameObject(name);
            go.transform.SetParent(_world, false);
            var r = go.AddComponent<SpriteRenderer>();
            r.sprite = _white;
            r.color = c;
            r.sortingOrder = order;
            return r;
        }

        private void Place(SpriteRenderer r, float x, float y, float w, float h)
        {
            Move(r, x, y);
            Size(r, w, h);
        }

        private void Move(SpriteRenderer r, float x, float y) =>
            r.transform.localPosition = new Vector3(x - _rules.field_w / 2f, _rules.field_h / 2f - y, 0f);

        private static void Size(SpriteRenderer r, float w, float h) => r.transform.localScale = new Vector3(w, h, 1f);

        private static Sprite MakeWhiteSprite()
        {
            // 4×4 흰 텍스처를 4 픽셀/단위로 만들어 스프라이트 한 장 = 월드 1×1. 크기는 transform 배율로 정한다.
            var tex = new Texture2D(4, 4, TextureFormat.RGBA32, false) { filterMode = FilterMode.Point, name = "White" };
            var px = new Color32[16];
            for (int i = 0; i < px.Length; i++)
                px[i] = new Color32(255, 255, 255, 255);
            tex.SetPixels32(px);
            tex.Apply();
            return Sprite.Create(tex, new UnityEngine.Rect(0, 0, 4, 4), new Vector2(0.5f, 0.5f), 4f);
        }
    }
}
