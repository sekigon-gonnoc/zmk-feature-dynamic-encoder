# nemu-link用の動的sensor割り当て

`sekigon,dynamic-encoder-source`で宣言したsourceごとに、layer別の割り当てrecordと倍率recordを生成します。既存のpointer encoder動作と保存済みrecord layoutを維持します。

本moduleはnemu-link用のZMK構成を前提とします。

## 導入（最小構成）

### west.yml

この例は既存の`west.yml`へ追記します。`sekigon-gonnoc` remoteが既にある場合は重ねて定義しません。nemu-link構成では、互換性のあるZMK forkと`zmk-feature-nemu-link`もmanifestに必要です。

```yaml
manifest:
  remotes:
    - name: sekigon-gonnoc
      url-base: https://github.com/sekigon-gonnoc
  projects:
    - name: zmk-feature-dynamic-settings
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
    - name: zmk-feature-dynamic-encoder
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
```

revisionには利用するZMK / nemu-link moduleと互換性のあるtagまたはcommitを指定し、manifest更新後に`west update`を実行します。

### .conf

```conf
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_STUDIO_RPC=y
CONFIG_ZMK_DYNAMIC_SETTINGS=y
CONFIG_SETTINGS_NVS=y
```

`CONFIG_SETTINGS_NVS`の代わりに、対象boardで利用するSettings backendを有効にできます。 `sekigon,dynamic-encoder-source` nodeがあると`CONFIG_ZMK_DYNAMIC_ENCODER_RUNTIME`が有効になります。

### overlay

`sensor`にはZMKのsensor device、`sensor-index`には`zmk,keymap-sensors`内のindexを指定します。`target-role`は設定を適用するrole名に置き換えてください。

```dts
/ {
    runtime_encoder_source: runtime_encoder_source {
        compatible = "sekigon,dynamic-encoder-source";
        source-name = "main";
        label = "Encoder";
        sensor = <&encoder_sensor>;
        sensor-index = <0>;
        input-mode = "tick";
        target-role = "keyboard-right";
    };

    runtime_encoder: runtime_encoder {
        compatible = "sekigon,dynamic-behavior-runtime-encoder";
        #binding-cells = <0>;
    };
};
```

`&encoder_sensor`は実際のsensor node labelに置き換え、sourceのsensor indexをkeymap sensor listと合わせます。keymapのsensor bindingで`&runtime_encoder`を使います。

## 追加設定

`input-mode`には`tick`または`angle`を指定し、sourceが出す値の形式をschemaで固定します。`tick`では`val2`をTrigger数としてそのまま処理します。`angle`では`val1/val2`を角度として積算し、`triggers-per-rotation`を適用します。後者のsettingはStudioの拡張設定画面にsourceごとに表示され、ZMKの`zmk,keymap-sensors`設定を初期値としてshared scopeに保存されます。hires dialのように1度未満の角度を出すセンサーは`angle`を指定してください。EC11など角度を出力するセンサーでは、センサーノードの`steps`に1回転あたりのパルス数を設定します。たとえば1回転20パルスで4パルスごとにTriggerする場合は`steps = <20>`、TPRを`<5>`にします。初期の時計回り・反時計回りbindingを設定する場合は`default-cw-binding`と`default-ccw-binding`を指定します。設定しないbindingはStudioから割り当てます。
