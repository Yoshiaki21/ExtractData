# CLAUDE.md

> このファイルはプロジェクトの進行に合わせて Claude Code 自身が更新していく「生きたドキュメント」です。
> 本プロジェクトは既存コード（git clone / fork したもの）です。作業を始める前に、下記「## 初回セットアップ（既存プロジェクト把握）」を必ず実行し、現状のコードベースを読み込んで各章を埋めてください。その後は通常の自己更新ルールに従って育てていきます。

---

## 初回セットアップ（既存プロジェクト把握）※このファイルを読んだら最初に一度だけ実行

このCLAUDE.mdの「## 構成」の各章がまだ空、または実態と大きくズレている場合、以下を行ってください。

1. リポジトリ全体のディレクトリ構造を走査する（Glob/Grepで俯瞰）
2. package.json / *.csproj / requirements.txt / README など設定・説明ファイルを読み、技術スタックを特定する
3. 既存の命名規則・レイヤー構成・設計パターンを、実コードから推測ではなく実例ベースで拾う
4. 「## 構成」の各章（1〜6）を、推測ではなく実際に確認できた情報のみで埋める
   - 特に「3. ディレクトリ構造 / Critical Paths」は、実際に存在する主要ディレクトリ・ファイルを根拠に表を作る
   - 不明な点・複数の流儀が混在している点は、正直に「（要確認）」「（新旧混在）」のように明記する。存在しないものを断定して書かない
5. 埋め終えたら、ユーザーに「初回スキャンが完了し、CLAUDE.mdを埋めました」と一言報告する

この初回セットアップが終わった後は、以下の「自己更新ルール」に従って通常運用に入ってください（再度全体スキャンをやり直す必要はなく、変更差分だけを追記していきます）。

---

## Claude Codeへの指示（自己更新ルール）

あなたはこのプロジェクトで作業するたびに、以下のルールに従って本ファイル（CLAUDE.md）を更新してください。

### 更新すべきタイミング
- 新しいファイル/モジュール/ディレクトリを作成したとき
- 既存のアーキテクチャや設計方針を変更したとき（例: ライブラリの乗り換え、レイヤー構成の変更）
- 「今後も同じ説明をユーザーにさせそうだ」と感じたとき
- ユーザーから明示的に「CLAUDE.mdに書いて」と言われたとき

### 更新時の振る舞い
- 該当する章（下記「## 構成」参照）に、簡潔な箇条書きで追記する。長文の説明文は書かない。
- 「Critical Paths（ファイル所在表）」は特に優先して最新化する。何かを追加/変更したら、まずここを疑う。
- 迷ったら、ユーザーに聞かずにいったん追記し、後で「CLAUDE.mdのここを追記しました」と一言報告する。
- 冗長な履歴は残さない。古い情報は書き換える（変更履歴セクション以外は追記ではなく上書き優先）。
- コード規約・設計判断の「理由」も一言添える（例: 「認証は express-session を採用（JWT不要な単一サーバー構成のため）」）。

### 書かないこと
- 実装の詳細なロジック説明（コード自体やdocstringに書くべきもの）
- 一時的なタスクの進捗（TODOリストや作業ログはここに書かない）
- 憶測・未確定の設計（決まったことだけを書く。初回セットアップ時に不明だった点は「（要確認）」のまま残し、断定で埋めない）

---

## 構成

### 1. プロジェクト概要

- 目的: 各種ゲーム（主に国産ノベルゲーム）のアーカイブファイルを開き、中身の一覧表示・抽出・画像/音声変換を行う Windows GUI ツール。Susie プラグイン経由の展開や、生バイナリからの画像/音声シグネチャ検索にも対応
- 想定ユーザー: ゲームのリソースを取り出したいエンドユーザー（ライセンスは GPLv2 — `gpl.txt` / `gpl.ja.txt`）
- リポジトリ: `origin` = github.com/Yoshiaki21/ExtractData（fork 元は要確認）

### 2. 技術スタック

- 言語/フレームワーク: C++（`LanguageStandard=stdcpplatest`, ConformanceMode 有効）、Win32 API 直叩き（MFC/ATL 不使用。COM は `wrl/client.h` のみ）
- ビルド: Visual Studio 2026（`ExtractData.sln`、ツールセット v145、Windows SDK は `10.0` = インストール済みの最新版）。構成は Debug/Release の **Win32 のみ**（x64 はソリューション上 Win32 にマップ）。本体・zlibvc・libpng の3つの vcxproj で同じ設定にそろえている
- 文字セット: MultiByte（`TCHAR` / `_T()` を使うが実体は ANSI/Shift-JIS）
- DB: なし（設定は INI: `ExtractData.ini`, `Susie.ini`）
- 主要ライブラリ: zlib 1.2.11（`Libs/zlib-1.2.11`、vc14 プロジェクト）、libpng 1.6.3（`Libs/lpng163`）、bzip2（`bzip2/` のソースを本体プロジェクトで直接コンパイル）、`winmm.lib`
- テスト: テストプロジェクト・CI は存在しない

### 3. ディレクトリ構造 / Critical Paths（最重要）

| やりたいこと | 場所 |
|---|---|
| アプリのエントリポイント / メインウィンドウ | `UI/WinMain.cpp`（`_tWinMain` → `CWinMain`） |
| 新しいアーカイブ形式（ゲーム）への対応を追加 | `Extract/<Name>.{h,cpp}` に `CExtractBase` 派生クラスを作成 → `Extract.cpp` の `#include` と `CExtract::SetClass()` に登録 → `ExtractData.vcxproj` と `.filters` に追加 |
| Giga NeXAS の `.pac`（末尾に Huffman 圧縮インデックスを持つ新形式） | `Extract/Nexas.{h,cpp}`（`CNexas`）。旧形式（先頭インデックス）は `Extract/Baldr.cpp` |
| NeXAS 立ち絵（`.spm` による本体＋表情差分の合成、表情名付きファイル名） | `Extract/Nexas.cpp` の `ComposeSprites()`（一覧作成）と `CNexas::Decode()`（合成・出力） |
| エンジン系列ごとの形式 | `Extract/krkr/`（吉里吉里）, `Extract/paz/`, `Extract/cpz/`, `Extract/TCD/` |
| 形式判定〜展開の全体フロー（Mount/Decode/Search の振り分け） | `Extract.cpp`（`CExtract`） |
| 形式クラスの基底インターフェース | `ExtractBase.h`（`Mount` 必須、`Decode`/`Extract` は任意） |
| 汎用の LZSS/zlib/画像/Ogg デコード共通実装 | `Extract/Standard.{h,cpp}`（`CStandard`）, `Arc/LZSS`, `Arc/Zlib` |
| アーカイブ読み込み・出力ファイル書き出し・ファイル情報登録 | `ArcFile.{h,cpp}`（`CArcFile`） |
| ファイル情報 `SFileInfo` / オプション `SOption` / ID 定数 | `Common.h` |
| 画像出力（BMP/PNG、α合成） | `Image.{h,cpp}`（`CImage`）, `Image/`（Bmp, Png, Tga, ImageBase） |
| 音声（Ogg の CRC 修正、Wav） | `Sound/` |
| 未知ファイルからのシグネチャ検索 | `Search/`（`CSearchBase` 派生）→ `CExtract::SetSearchClass()` に登録 |
| Susie プラグイン連携 | `Susie.{h,cpp}`、API 資料は `Docs/Susie Plugin API Rev4+α.md` |
| 独自の基盤クラス（文字列/ファイル/INI/DLL） | `Base/`（`YCString`, `YCFile`, `YCIni`, `YCLibrary` など） |
| UI（リストビュー/ツールバー/オプション画面/進捗） | `UI/`、汎用コントロールは `UI/Ctrl/`、ダイアログは `UI/Dialog/` |
| 設定の読み書き | `UI/Option.cpp`（`COption::LoadIni/SaveIni`） |
| 型エイリアス（`u8`〜`u64`, `s8`〜`s64`） | `Types.h` |
| バイトスワップ/ローテート | `Utils/BitUtils.{h,cpp}` |
| プリコンパイル済みヘッダ（全 .cpp の先頭で include 必須） | `StdAfx.h` |
| リソース（アイコン・ツールバー画像・rc） | `res/` |

### 4. 設計方針・規約

- 形式対応は「1形式=1クラス」のプラグイン方式。`SetClass()` で static インスタンスを `m_class` に登録し、登録順に `Mount()` を試して最初に成功したものを採用する（順序に意味がある。例: `CKrkr` は吉里吉里派生形式の後、`CImage`/`COgg` は最後）
- 新しめのコード: 派生クラスは `final` + `override`、戻り値は `bool`、引数は snake_case（`CArcFile* archive`）、メンバは `m_` + snake_case、実装詳細は cpp の無名 namespace に置く、`std::vector`/`std::array`/`<algorithm>` を使う
- 古いコード（新旧混在）: ハンガリアン記法（`pclArc`, `szXxx`, `bXxx`, `clXxx`）、`BOOL`/`DWORD`/`TRUE`、生配列。`SOption` などは旧記法のまま
- 直近のコミットは旧記法→新記法、Windows 型→`u32` 等への段階的リファクタが中心。コミットメッセージは `<クラス/ファイル名>: <変更内容>` 形式（例: `Krkr: Utilize std::any_of within CheckTpm()`）
- インデントは基本タブ（`Image.h` などスペース4のファイルもあり — 新旧混在）
- `NOMINMAX` を定義済み（`std::min/max` を使うため Windows の min/max マクロを除外）

### 5. よく使うコマンド

```bash
# VS 2026 の MSBuild でビルド（Release/Debug とも 2026-09-27 動作確認済み）
"/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" ExtractData.sln -p:Configuration=Release -p:Platform=Win32 -m -v:m
```

- 出力先は `Release/`（`ExtractData.exe`, `zlib.dll`, `libpng16.dll`）と `Debug/`（`ExtractDataDebug.exe`, `zlibd.dll`, `libpng16d.dll`）。exe は DLL と同じフォルダで実行する
- `Release/` だけを手で消すと libpng の BSCMAKE が `BK1513` で失敗する（中間ファイルが `Libs/lpng163/projects/vstudio/libpng/Release/` に残るため）。その場合は `-t:Rebuild` を付ける
- テスト実行コマンドは存在しない

### 6. 既知の制約・注意点

- Windows / 32bit 専用（README の長期 TODO に Linux/macOS 対応あり）。x64 構成は実質無効
- リポジトリにあるが vcxproj に含まれずビルドされないファイル: `Extract/FateFD.cpp`（旧版。現行は `Extract/krkr/FateFD`）, `Image/Jpg.cpp`, `Ini.cpp`, `UI/DataBase/*`, `UI/DragDrop/*`（意図的に外しているかは要確認）
- `Crx` 形式は `Extract.cpp` 内でコメントアウトされ無効
- `CArcFile::SeekEnd(offset)` は内部で `-offset` にして末尾基準でシークする。末尾から 4 バイト戻るなら `SeekEnd(4)`（`fseek` と同じ感覚で負の値を渡すと末尾より後ろに行く）
- 変換して出力する画像の形式は、一覧の拡張子ではなく設定（Extraction Settings → Output image format、`SOption::bDstBMP`/`bDstPNG`）で決まる。初期値は BMP（`UI/Option.cpp`）。一覧で `.png` と表示されていても BMP で出るのは仕様
- 形式クラスで `SFileInfo::format = _T("zlib")` にすると、`Arc/Zlib.cpp`（`CZlib`）が展開し、名前が `.bmp` なら画像変換まで行う。独自の Decode を書く前にこれで足りるか確認する
- `.spm` を含む NeXAS `.pac` は、部品 PNG と `.spm` を一覧から隠し、フレームごとの合成画像だけを出す（ユーザー要望: 部品は不要）。変換なしの抽出でも合成画像を出力する。名前は `<最前面の部品名>_<表情名>[_B].png`（`_B` は部品名の末尾に `_` が付く版。意味は要確認。表情名は `_` なし側の `.spm` から借りる）
- 合成エントリーは `format = "SPM"`、`key` = フレーム番号、`starts`/`sizes_*`/`compress_checks` の [0] が `.spm`、[1..] が部品（`Himauri` と同じ流儀）
- `CPng::Decompress()` はスタブで PNG 読み込み機能は無い。PNG のデコードは libpng の簡易 API（`png_image_*_read_from_memory`）を直接使う
- ソースは BOM なし UTF-8 だが `/utf-8` 無しの MultiByte ビルドなので、ソース（コメント含む）に日本語を書かない（Shift-JIS と誤解釈される）
- `CExtract::m_decode_class` は全アーカイブ共通の static な集合なので、`Decode()`/`Extract()` は自分のエントリーか（`format`・拡張子・ヘッダー）を必ず確認して、違えば `false` を返す
- `CNexas` と `CBaldr` はどちらも `.pac` + `"PAC"` で判定するため、`SetClass()` では `CNexas` を先に登録する（`CNexas` はインデックスを全件検証してから登録するので、旧形式は `CBaldr` に回る）
- ファイルを追加したら `ExtractData.vcxproj` と `ExtractData.vcxproj.filters` の両方を手動で更新する必要がある
- 文字セットが MultiByte のため、ファイル名は Shift-JIS 前提。ソース内に日本語コメントあり（ファイルごとのエンコーディングは要確認）
- zlib の vc14 プロジェクトは MASM ファイル（`Libs/zlib-1.2.11/contrib/masmx86`, `masmx64`）を参照する

### 7. 変更履歴（任意）

- 2026-09-27: CLAUDE.md 初回セットアップ（既存コードのスキャン結果を記入）
- 2026-09-27: VS2017 (v141, SDK 10.0.15063/17763) から VS2026 (v145, SDK 10.0) へ再ターゲット（旧 SDK が VS2026 で入手できないため）。廃止済みの `/Gm`（MinimalRebuild）が `/std:c++latest` と衝突するため Debug で無効化
- 2026-09-27: NeXAS 新形式 `.pac` に対応（`CNexas`）。`CZlib::Decompress(u8*, u32*, ...)` が出力サイズを 0 で渡していた既存バグ（2019-02 のリファクタで混入）を修正
- 2026-09-28: NeXAS `.pac` 内の `.spm` を解析し、立ち絵を合成済み画像（表情名付き）として一覧・出力するよう変更

---

**運用メモ（ユーザー向け）**
- 初回スキャンの結果は完璧ではありません。特に「3. Critical Paths」と「4. 設計方針」は、実際に何か修正を依頼したときの挙動を見ながら、都度手直ししてください。
- ある程度育ってきたら、章ごとに `docs/` 配下へ分割してこのファイルからリンクする構成に移行することを検討してください（tududiのCLAUDE.mdのような形）。
- 定期的に「CLAUDE.mdを見直して、実態と合っていない箇所を修正して」とClaude Codeに依頼すると、陳腐化を防げます。
