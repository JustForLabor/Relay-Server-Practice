using System;
using System.Collections.Generic;
using System.Reflection;
using Pong.Game;
using Pong.Protocol;
using Relay.Lobby;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.InputSystem.UI;
using UnityEngine.Rendering.Universal;
using UnityEngine.SceneManagement;
using UnityEngine.UI;

namespace Relay.EditorTools
{
    /// <summary>
    /// Main.unity를 코드로 만든다(씬 YAML을 손으로 고치면 fileID · GUID를 틀리기 쉽다).
    /// batch: unity run &lt;project&gt; -- -executeMethod Relay.EditorTools.SceneBuilder.Build
    /// 검사: ... -executeMethod Relay.EditorTools.SceneBuilder.Verify (어긋나면 예외 → 종료 코드 1)
    /// </summary>
    public static class SceneBuilder
    {
        public const string ScenePath = "Assets/Scenes/Main.unity";
        private const string TemplateScene = "Assets/Scenes/SampleScene.unity";

        [MenuItem("Relay/Build Main Scene")]
        public static void Build()
        {
            Scene scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);

            var camGo = new GameObject("Main Camera") { tag = "MainCamera" };
            var cam = camGo.AddComponent<Camera>();
            cam.orthographic = true;
            cam.orthographicSize = 330f;
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.backgroundColor = Color.black;
            camGo.transform.position = new Vector3(0f, 0f, -10f);
            camGo.AddComponent<AudioListener>();
            camGo.AddComponent<UniversalAdditionalCameraData>();

            // URP 2D 렌더러의 기본 스프라이트 재질(Sprite-Lit-Default)은 2D 조명이 있어야 밝게 그려진다.
            var lightGo = new GameObject("Global Light 2D");
            var light = lightGo.AddComponent<Light2D>();
            light.lightType = Light2D.LightType.Global;
            light.intensity = 1f;
            light.color = Color.white;

            var canvasGo = new GameObject("UI", typeof(RectTransform), typeof(Canvas), typeof(CanvasScaler), typeof(GraphicRaycaster));
            var canvas = canvasGo.GetComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            var scaler = canvasGo.GetComponent<CanvasScaler>();
            scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
            scaler.referenceResolution = new Vector2(800f, 600f);
            scaler.matchWidthOrHeight = 0.5f;

            // 프로젝트는 Input System 전용(activeInputHandler 1)이라 옛 StandaloneInputModule은 쓸 수 없다.
            new GameObject("EventSystem", typeof(EventSystem), typeof(InputSystemUIInputModule));

            var gmGo = new GameObject("GameManager");
            gmGo.AddComponent<LobbyController>();
            var gm = gmGo.AddComponent<GameManager>();
            var so = new SerializedObject(gm);
            so.FindProperty("fieldCamera").objectReferenceValue = cam;
            so.FindProperty("uiRoot").objectReferenceValue = canvasGo.GetComponent<RectTransform>();
            so.ApplyModifiedPropertiesWithoutUndo();

            if (!EditorSceneManager.SaveScene(scene, ScenePath))
                throw new Exception("SaveScene failed: " + ScenePath);

            // 빌드에는 Main 하나만 넣는다. 템플릿의 빈 SampleScene은 남겨 두면 어느 씬이 진짜인지 헷갈린다.
            EditorBuildSettings.scenes = new[] { new EditorBuildSettingsScene(ScenePath, true) };
            if (AssetDatabase.LoadAssetAtPath<SceneAsset>(TemplateScene) != null)
                AssetDatabase.DeleteAsset(TemplateScene);

            // 두 창을 나란히 띄워 대전하므로 창 모드 · 백그라운드 실행이 기본이어야 한다.
            PlayerSettings.productName = "Relay Pong";
            PlayerSettings.runInBackground = true;
            PlayerSettings.visibleInBackground = true;
            PlayerSettings.fullScreenMode = FullScreenMode.Windowed;
            PlayerSettings.defaultScreenWidth = 800;
            PlayerSettings.defaultScreenHeight = 600;
            PlayerSettings.resizableWindow = true;
            AssetDatabase.SaveAssets();
            Debug.Log("[SceneBuilder] built " + ScenePath);
        }

        [MenuItem("Relay/Verify Main Scene")]
        public static void Verify()
        {
            var problems = new List<string>();
            Scene scene = EditorSceneManager.OpenScene(ScenePath, OpenSceneMode.Single);
            if (!scene.IsValid())
                throw new Exception("cannot open " + ScenePath);

            GameManager gm = UnityEngine.Object.FindFirstObjectByType<GameManager>();
            if (gm == null)
                throw new Exception("GameManager component not found in " + ScenePath);
            if (gm.GetComponent<LobbyController>() == null)
                problems.Add("LobbyController missing on GameManager object");

            // 디스크에 저장된 값을 본다: SerializedObject는 씬 파일에서 읽은 직렬화 값을 그대로 보여 준다.
            var so = new SerializedObject(gm);
            var defaults = new PongSettings();
            int checkedCount = 0;
            foreach (FieldInfo f in typeof(PongSettings).GetFields(BindingFlags.Public | BindingFlags.Instance))
            {
                SerializedProperty p = so.FindProperty(f.Name);
                int expect = (int)f.GetValue(defaults);
                if (p == null)
                    problems.Add($"field {f.Name} not serialized");
                else if (p.intValue != expect)
                    problems.Add($"{f.Name} = {p.intValue}, expected {expect}");
                else
                    checkedCount++;
                Debug.Log($"[SceneBuilder] {f.Name} = {(p == null ? "missing" : p.intValue.ToString())} (default {expect})");
            }
            if (checkedCount != 13)
                problems.Add($"{checkedCount}/13 rule values match");
            if (so.FindProperty("fieldCamera").objectReferenceValue == null)
                problems.Add("fieldCamera not assigned");
            if (so.FindProperty("uiRoot").objectReferenceValue == null)
                problems.Add("uiRoot not assigned");
            if (so.FindProperty("serverHost").stringValue != "127.0.0.1" || so.FindProperty("serverPort").intValue != 7777)
                problems.Add("server address is not 127.0.0.1:7777");
            // "UDP 사용" 토글(protocol.md 7장): 기본은 켬. 끄면 TCP만 쓴다.
            SerializedProperty useUdp = so.FindProperty("useUdp");
            if (useUdp == null || useUdp.propertyType != SerializedPropertyType.Boolean)
                problems.Add("useUdp toggle not serialized");
            else if (!useUdp.boolValue)
                problems.Add("useUdp is off, expected on (default)");
            Debug.Log($"[SceneBuilder] useUdp = {(useUdp == null ? "missing" : useUdp.boolValue.ToString())} (default True)");
            if (UnityEngine.Object.FindFirstObjectByType<EventSystem>() == null)
                problems.Add("EventSystem missing");
            if (UnityEngine.Object.FindFirstObjectByType<Light2D>() == null)
                problems.Add("Global Light 2D missing");

            EditorBuildSettingsScene[] scenes = EditorBuildSettings.scenes;
            if (scenes.Length == 0 || scenes[0].path != ScenePath || !scenes[0].enabled)
                problems.Add("Main.unity is not the first enabled scene in Build Settings");

            if (problems.Count > 0)
                throw new Exception("[SceneBuilder] verify failed:\n" + string.Join("\n", problems));
            Debug.Log($"[SceneBuilder] verify OK: GameManager with {checkedCount}/13 default rule values, useUdp on, build scene {ScenePath}");
        }
    }
}
