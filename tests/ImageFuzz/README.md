# ImageFuzz

現在の製品形式である `.tsumugi` v1、その認証対象manifest、埋込みpartition snapshotを対象にした、依存追加なしの決定論的ファズ・スモークです。

`ytec-image-fuzz-tests` は不正・境界seed 5件と`.tsumugi` v1 Golden corpus 17件をメモリ上だけで変異させ、各inspect APIへ渡します。Golden seedはMBR／GPT-512e、exact／shrink／rescue、暗号化を含みます。manifestとpartition snapshotが受理された場合は、再エンコード結果が入力と完全一致することも検査します。形式別counterにより、17件のseedがcontainer 9件、manifest 5件、snapshot 3件の全分類へ到達することを通常実行の開始時に固定検証します。

実行境界は次のとおり固定しています。

- seed: `0x5954454346555A5A`
- 変異: 4,096回（seed 22件を含めて合計4,118件）
- 1入力の上限: 64 KiB
- CTest timeout: 120秒
- 通常CTestでのファイル、物理ディスク、USB、VM、ネットワークへのI/O: なし

通常版では次を実行します。

```powershell
. .\scripts\Enter-MsvcEnvironment.ps1
cmake --preset msvc-x64
cmake --build --preset msvc-x64 --target ytec-image-fuzz-tests
ctest --test-dir out\build\msvc-x64 -R '^ytec-image-fuzz-tests$' --output-on-failure
```

AddressSanitizer版ではpresetを `msvc-x64-asan` に置き換えます。暗号化Golden seedの正当passwordはseed自体の到達確認時にだけ使用し、各変異で高コストKDFを再試行しません。これは長時間のcoverage-guided fuzzingやlibFuzzerの代替ではなく、CTest、`/analyze /WX`、AddressSanitizerで毎回再現できる最小基盤です。旧 `.dcimg` / `.dcmig` 形式は製品経路から隔離されているため対象外です。

ローカル環境で反復数を増やす場合は、次の決定的soak runnerを使えます。16個の固定seedから指定batch数を順に実行し、失敗したseedをそのまま再実行できます。各campaignは最大100,000,000変異、入力は引き続き64 KiB以内です。

```powershell
cmake --build --preset msvc-x64-asan --target ytec-image-fuzz-tests
.\tests\ImageFuzz\Run-DeterministicImageSoak.ps1 `
  -Preset msvc-x64-asan `
  -IterationsPerBatch 250000 `
  -BatchCount 8
```

runnerと実行結果は`coverage_guided=false`を明示します。coverage feedback、corpus minimization、crash artifact管理を持つlibFuzzer等の長時間coverage-guided fuzzingは別ゲートであり、この決定的soakをその実施済み証拠にはできません。

MSVC 19.50以降と同梱libFuzzer runtimeがある環境では、opt-inのcoverage-guided targetを使用できます。専用build directoryでparser-onlyの`ImageFormat` twinとharnessだけに`/fsanitize=fuzzer`を付けるため、製品`ImageFormat` targetや通常test executableはcoverage instrumentationの影響を受けません。次はGolden 17件をSHA-256／長さ照合後にbinary corpusへ取り込み、既存corpusを維持したまま1時間実行します。暗号化Goldenの正当password経路は各campaignの開始時に1入力だけをASanでpreflightし、本走ではpasswordを渡しません。これにより、Argon2idの意図した高コスト作業領域を同一プロセスで数百万回反復してsanitizer allocatorを圧迫することなく、暗号化成功経路と長時間parser campaignを別々に証明します。

```powershell
.\tests\ImageFuzz\Run-CoverageGuidedImageFuzz.ps1 `
  -MaxTotalTimeSeconds 3600 `
  -MaximumInputBytes 65536
```

libFuzzerの出力に`Loaded ... inline 8-bit counters`、coverage／feature増加、最終統計が記録されることを確認します。corpusとcrash artifactはそれぞれ`out/fuzz/image-v1/corpus`、`out/fuzz/image-v1/artifacts`へ残ります。短時間smokeはharnessがcoverage-guidedで動く証拠ですが、リリースゲートとしての長時間campaign実施済み証拠ではありません。またこのharnessは不正なメモリ入力に対するReader境界であり、VM、物理ディスク、USB、実復元を検証しません。

通常CTestから到達しないGolden fixture監査用の明示引数だけを同じ開発バイナリに境界化しています。これは製品へリンクされず、既存fixtureを上書きしません。利用条件と恒久fixture追加手順は`tests/ImageGolden/README.md`を参照してください。
