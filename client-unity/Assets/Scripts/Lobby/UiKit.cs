using System;
using UnityEngine;
using UnityEngine.UI;

namespace Relay.Lobby
{
    /// <summary>
    /// uGUI를 코드로 만드는 작은 도우미. 화면이 단순해 프리팹 · 씬 YAML보다 코드가 고치기 쉽다.
    /// 배치는 LayoutGroup에 맡겨 좌표 계산을 줄인다.
    /// </summary>
    public static class UiKit
    {
        private static Font _font;

        /// <summary>
        /// 한글이 보이는 글꼴. 기본 LegacyRuntime.ttf(Arial 계열)에는 한글 글리프가 없어
        /// Windows 기본 한글 글꼴(맑은 고딕)을 OS에서 직접 읽는다. 없는 기계에서는 다음 이름으로 넘어간다.
        /// </summary>
        public static Font Font
        {
            get
            {
                if (_font == null)
                    _font = Font.CreateDynamicFontFromOSFont(new[] { "Malgun Gothic", "맑은 고딕", "Gulim", "Arial" }, 18);
                if (_font == null)
                    _font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
                return _font;
            }
        }

        // 불투명: 경기장 위에 뜨는 결과 · 대기 패널 뒤로 중앙선이 비치면 글자가 그어진 것처럼 보인다.
        public static readonly Color PanelColor = new Color(0.08f, 0.09f, 0.12f, 1f);
        public static readonly Color ButtonColor = new Color(0.22f, 0.36f, 0.62f, 1f);
        public static readonly Color ErrorColor = new Color(1f, 0.45f, 0.4f, 1f);

        public static RectTransform Child(Transform parent, string name)
        {
            var go = new GameObject(name, typeof(RectTransform));
            go.transform.SetParent(parent, false);
            return (RectTransform)go.transform;
        }

        /// <summary>
        /// 부모 안에서 가운데에 놓이는 패널. 폭은 고정, 높이는 안의 내용에 맞춘다:
        /// 비공개 방 코드 줄처럼 상황에 따라 숨는 줄이 있어 고정 높이면 아래가 비어 보인다.
        /// </summary>
        public static RectTransform Panel(Transform parent, string name, float width, Vector2 anchor)
        {
            RectTransform rt = Child(parent, name);
            rt.anchorMin = rt.anchorMax = anchor;
            rt.pivot = anchor;
            rt.sizeDelta = new Vector2(width, 0f);
            rt.anchoredPosition = Vector2.zero;
            rt.gameObject.AddComponent<ContentSizeFitter>().verticalFit = ContentSizeFitter.FitMode.PreferredSize;
            rt.gameObject.AddComponent<Image>().color = PanelColor;
            var v = rt.gameObject.AddComponent<VerticalLayoutGroup>();
            v.padding = new RectOffset(16, 16, 14, 14);
            v.spacing = 8;
            v.childControlWidth = true;
            v.childControlHeight = true;
            v.childForceExpandWidth = true;
            v.childForceExpandHeight = false;
            return rt;
        }

        /// <summary>가로 한 줄. 안의 요소는 폭을 나눠 갖는다(LayoutElement로 고정 폭을 줄 수 있다).</summary>
        public static RectTransform Row(Transform parent, float height)
        {
            RectTransform rt = Child(parent, "Row");
            var h = rt.gameObject.AddComponent<HorizontalLayoutGroup>();
            h.spacing = 8;
            h.childControlWidth = true;
            h.childControlHeight = true;
            h.childForceExpandWidth = true;
            h.childForceExpandHeight = true;
            Height(rt, height);
            return rt;
        }

        public static void Height(Component c, float h)
        {
            var le = Element(c);
            le.minHeight = h;
            le.preferredHeight = h;
        }

        public static void Width(Component c, float w)
        {
            var le = Element(c);
            le.minWidth = w;
            le.preferredWidth = w;
            le.flexibleWidth = 0;
        }

        // GetComponent()??는 쓰지 않는다: Editor에서 없는 컴포넌트는 C# null이 아닌 "가짜 null"이 와서 ??가 통과한다.
        private static LayoutElement Element(Component c) =>
            c.TryGetComponent(out LayoutElement le) ? le : c.gameObject.AddComponent<LayoutElement>();

        public static Text Label(Transform parent, string text, int size, TextAnchor align = TextAnchor.MiddleLeft)
        {
            RectTransform rt = Child(parent, "Label");
            var t = rt.gameObject.AddComponent<Text>();
            t.font = Font;
            t.fontSize = size;
            t.alignment = align;
            t.color = Color.white;
            t.text = text;
            t.horizontalOverflow = HorizontalWrapMode.Wrap;
            t.verticalOverflow = VerticalWrapMode.Truncate;
            t.raycastTarget = false;
            Height(t, size + 10);
            return t;
        }

        /// <summary>부모 전체를 덮는 글자(점수판 등). 레이아웃 그룹 밖에서 앵커로 놓을 때 쓴다.</summary>
        public static Text Overlay(Transform parent, string name, int size, TextAnchor align, Vector2 anchorMin, Vector2 anchorMax)
        {
            RectTransform rt = Child(parent, name);
            rt.anchorMin = anchorMin;
            rt.anchorMax = anchorMax;
            rt.offsetMin = new Vector2(10, 6);
            rt.offsetMax = new Vector2(-10, -6);
            var t = rt.gameObject.AddComponent<Text>();
            t.font = Font;
            t.fontSize = size;
            t.alignment = align;
            t.color = Color.white;
            t.raycastTarget = false;
            t.horizontalOverflow = HorizontalWrapMode.Overflow;
            return t;
        }

        public static Button Button(Transform parent, string text, Action onClick)
        {
            RectTransform rt = Child(parent, "Button");
            var img = rt.gameObject.AddComponent<Image>();
            img.color = ButtonColor;
            var b = rt.gameObject.AddComponent<Button>();
            b.targetGraphic = img;
            b.onClick.AddListener(() => onClick());
            Text t = Label(rt, text, 18, TextAnchor.MiddleCenter);
            Stretch((RectTransform)t.transform);
            Height(b, 38);
            return b;
        }

        public static void SetButtonText(Button b, string text) => b.GetComponentInChildren<Text>().text = text;

        public static InputField Input(Transform parent, string placeholder, int limit)
        {
            RectTransform rt = Child(parent, "Input");
            rt.gameObject.AddComponent<Image>().color = new Color(0.9f, 0.9f, 0.92f, 1f);
            var field = rt.gameObject.AddComponent<InputField>();
            Text ph = Label(rt, placeholder, 18, TextAnchor.MiddleLeft);
            ph.color = new Color(0.4f, 0.4f, 0.45f, 1f);
            Inset((RectTransform)ph.transform);
            Text tx = Label(rt, "", 20, TextAnchor.MiddleLeft);
            tx.color = Color.black;
            tx.supportRichText = false;
            Inset((RectTransform)tx.transform);
            field.textComponent = tx;
            field.placeholder = ph;
            field.characterLimit = limit;
            Height(field, 38);
            return field;
        }

        /// <summary>세로 목록을 담는 스크롤 영역. 반환값은 항목을 넣을 content.</summary>
        public static RectTransform ScrollList(Transform parent, float height)
        {
            RectTransform root = Child(parent, "Scroll");
            root.gameObject.AddComponent<Image>().color = new Color(0f, 0f, 0f, 0.35f);
            var scroll = root.gameObject.AddComponent<ScrollRect>();
            Height(root, height);

            RectTransform viewport = Child(root, "Viewport");
            Stretch(viewport);
            viewport.gameObject.AddComponent<RectMask2D>();

            RectTransform content = Child(viewport, "Content");
            content.anchorMin = new Vector2(0, 1);
            content.anchorMax = new Vector2(1, 1);
            content.pivot = new Vector2(0.5f, 1);
            content.sizeDelta = Vector2.zero;
            var v = content.gameObject.AddComponent<VerticalLayoutGroup>();
            v.padding = new RectOffset(6, 6, 6, 6);
            v.spacing = 4;
            v.childControlWidth = true;
            v.childControlHeight = true;
            v.childForceExpandWidth = true;
            v.childForceExpandHeight = false;
            content.gameObject.AddComponent<ContentSizeFitter>().verticalFit = ContentSizeFitter.FitMode.PreferredSize;

            scroll.viewport = viewport;
            scroll.content = content;
            scroll.horizontal = false;
            scroll.movementType = ScrollRect.MovementType.Clamped;
            return content;
        }

        public static void Stretch(RectTransform rt)
        {
            rt.anchorMin = Vector2.zero;
            rt.anchorMax = Vector2.one;
            rt.offsetMin = Vector2.zero;
            rt.offsetMax = Vector2.zero;
        }

        private static void Inset(RectTransform rt)
        {
            Stretch(rt);
            rt.offsetMin = new Vector2(10, 2);
            rt.offsetMax = new Vector2(-10, -2);
        }
    }
}
