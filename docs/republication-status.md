# 再公開の範囲 / Republication Scope

確認日 / Checked: 2026-09-29

対象版 / Version: `1.0.0-internal-beta`

## 公開範囲 / Publication Scope

今回の再公開はソースと文書だけです。製品ZIPの一般配布は保留中です。代表実機の受入未完了を
変更せず、正式版1.0.0、実機安全性、OS互換性を新たに保証しません。
マニュアルは所有者が確認し、ソース・文書の公開を承認しています。

This is a source-and-document republication. General product-ZIP
distribution is on hold. Representative hardware acceptance remains incomplete;
this work does not declare stable 1.0.0, hardware safety or new OS compatibility.
The owner reviewed the manuals and approved source-and-document publication.

## 固定ソースと既存変更 / Pinned Source and Existing Work

現行Private正本から、Git履歴・旧成果物を持ち込まずに選択ソースを取り出しました。
元フォルダーの既存変更は保存し、英語UI・アイコン・関連テストは勝手に統合しません。
英語マニュアルだけは既存の文書草稿を別に確認して採用しています。画面名の英訳は
日本語UIの説明用です。一般製品公開前に、必要な英語UI変更の採否・検証を別途判断します。

Selected source is exported from the current private canonical without old Git
history or artifacts. Original changes are preserved. English UI, artwork and
related test changes are not silently merged. Only the existing English manual
draft is separately reviewed as documentation. Its translated screen names
describe the Japanese pinned UI. The bilingual UI work requires a separate
selection and validation decision before general product publication.

## 過去の失敗 / Historical Failures

初回の通常クローン実機試験は、データ複製と換装後起動に成功した一方、製品完了判定が
error 1168で失敗しました。修正版の同構成再試験まで合格扱いにしません。
局所テストの初期失敗名（`ytec-windows-resume-slot-platform-tests`等）も元の証跡を
保持しています。後続のPASSログだけから、初回失敗原因や解消を断定しません。
旧VirtualBoxの試験結果は履歴資料であり、現在のVMwareラボの合格と読み替えません。

The first hardware clone test copied data and booted after replacement, but its
product completion judgment failed with error 1168. The fixed path requires the
same-configuration retest. Earlier local failures, including
`ytec-windows-resume-slot-platform-tests`, remain preserved. Later PASS logs do
not establish the initial failure cause or resolution. Legacy VirtualBox results
are historical and are not new VMware-lab acceptance.

## 案内 / Links

- 紹介 / Product: <https://ytec.cloudfree.jp/forge/projects/tsumugi-drive/>
- ソース / Source: <https://github.com/ytec-forge-commits/ytec-disk-clone>
- 連絡 / Contact: <https://ytec.cloudfree.jp/forge/contact/>
