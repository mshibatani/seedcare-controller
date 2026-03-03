# seedcare-controller

育苗のためのヒーター自動制御 + 土壌水分モニタリングシステム。

## 概要

- ESP32で温度センサーとリレーを使い、設定温度の範囲でヒーターをON/OFFする。
- 土壌水分センサーの値も同時にモニタリングし、すべてのデータをMQTTでRaspberry Piのサーバーに送信し記録する。
-- https://github.com/mshibatani/seedcare #サーバー側のモニタープロジェクト
- Webサーバーを内蔵しており、現在のデーターやファームウェアアップデートが行える。

## ハードウェア構成

| 部品 | 型番・仕様 | 接続先 |
|------|-----------|--------|
| マイコン | ESP32 Dev Module (秋月) | - |
| 温度センサー | DS18B20 (OneWire) x2 | GPIO 32 |
| 土壌水分センサー | 静電容量式 x2 | A6, A7 |
| リレーモジュール | x2 | GPIO 18, 19 |
| LCD | ST7032 (16x2, I2C) | SDA/SCL |

## 使用ヒーターと使用例

Amazonで購入した安価な電熱マットを使用しています。
https://www.amazon.co.jp/s?k=電熱マット+園芸
消費電力はピークで10Wh/h、平均8Wh/h程度です。
DIYで買える発泡スチロール製のボックスに入れて使用しています。
昼間はフタを開けて日光を取り入れ、夜や雨天時に閉じるというふうに運用しています。

## 回路図

![回路図](docs/schematic_diagram.pdf)

## 動作仕様

### 温度制御（10秒周期）

| リレー | ON条件 | OFF条件 |
|--------|--------|---------|
| Relay 0 | < 27.5°C | > 28.0°C |
| Relay 1 | < 22.5°C | > 23.0°C |

ヒステリシス付きで、閾値の間では現在の状態を維持する。

### 通信・UI

- **Web UI** (`http://<IP>/`) — 温度・水分の表示、ヒーターの手動ON/OFF
- **MQTT** — 10秒ごとに温度・水分・リレー状態を publish
- **OTA更新** (`http://<IP>/update`) — ブラウザからファームウェア更新
- **クラッシュログ** (`http://<IP>/logs`) — 異常停止時のチェックポイント履歴

### LCD表示

```
27.50C° 45% ON
22.80C° 38% OFF
```
WiFi未接続時は `*` マークが表示される。

## セットアップ

1. `include/config.h.example` を `include/config.h` にコピーし、WiFi/MQTTの設定を記入
2. PlatformIOでビルド＆アップロード: `pio run -t upload`
3. 2回目以降はOTA (`http://<IP>/update`) でも更新可能

## ファイル構成

```
src/main.cpp          メインプログラム
include/config.h      WiFi/MQTT設定（.gitignore済み）
include/config.h.example  設定テンプレート
platformio.ini        PlatformIO設定
docs/                 設計メモ
docs/schematic_diagram.pdf  回路図(参考)
```

## 課題
静電容量式の水分センサーは土の密度などで容易に上下し、あまり正確な数値が分からず、管理は断念してしまいました。

