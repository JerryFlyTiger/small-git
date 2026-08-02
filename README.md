# small_git (sg)

用純 C 打造、與 git 物件格式相容的另一款版本控制工具,目標是解決 git 常見的四類痛點:
CLI/UX 混亂、危險操作缺乏救援機制、大型/二進位檔案處理、巨型 repo 效能。

詳細設計與路線圖見 [docs/DESIGN.md](docs/DESIGN.md)。

## 建置

```sh
make          # 建置 build/sg
make test     # 執行測試
make sanitize # 以 ASan/UBSan 建置並跑測試
```

需要系統已安裝 `zlib`、`openssl`、`libcurl`(均透過 pkg-config 偵測)。

## 現況

Phase 5b(smart HTTP clone/fetch,唯讀方向)。
