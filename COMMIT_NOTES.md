# Ghi chú khi commit / push repo này

> Ghi bởi Claude Code, 2026-06-15. Đọc trước khi commit toàn bộ repo.

## ⚠️ QUAN TRỌNG: file bị gitignore nhưng PHẢI commit

Fix gộp cookie (Set-Cookie) cho tính năng nhạc Zing nằm ở:

```
managed_components/78__esp-ml307/src/http_client.cc
```

Thư mục `managed_components/` **bị gitignore** → file này KHÔNG tự được add.
Nếu không add, fix sẽ MẤT khi component được fetch lại / checkout sạch, và
search Zing sẽ hỏng (thiếu cookie zmp3_rqid → lỗi `err=-201`).

**Bắt buộc force-add khi commit:**

```bash
git add -f managed_components/78__esp-ml307/src/http_client.cc
```

## Lệnh commit toàn bộ

```bash
cd "D:/TAILIEU/MyProject/firmware esp/xiaozhi ai/xiaozhi-esp32"
rm -f .git/index.lock                 # nếu có lock cũ treo (do agent-bridge hook) hoặc del /f .git\index.lock (CMD)
git add -A
git add -f managed_components/78__esp-ml307/src/http_client.cc
git commit -m "Local customizations: clock home, PC video stream, YouTube storyboard, Zing music + now-playing UI"
```

## Đẩy lên GitHub mới của bạn (chưa có gh CLI)

1. Tạo repo RỖNG trên github.com (vd. `teeho88/xiaozhi-es3c28p`), KHÔNG thêm README/license.
2. Thêm remote và push (giữ nguyên ~914 commit lịch sử upstream + commit mới):

```bash
git remote add myfork https://github.com/teeho88/xiaozhi-es3c28p.git
git push -u myfork main
```

## Lưu ý môi trường

- Có agent-bridge hook thỉnh thoảng chạy `git` và giữ `.git/index.lock`,
  làm `git add` của tiến trình khác bị chặn. Nếu `git add` không ăn (staged = 0),
  xóa `.git/index.lock` rồi thử lại, hoặc chạy lệnh git trực tiếp trong terminal
  của bạn (tiền tố `!` trong Claude Code).
- Repo git thật sự là thư mục `xiaozhi-esp32/` (không phải workspace root).

## Tóm tắt thay đổi trong đợt này (tính năng nhạc Zing — ĐÃ XONG & verify)

- Sửa 5 bug: gzip (giải nén bằng miniz), cookie timing (GetStatusCode trước
  CaptureCookie), cookie merge (gộp nhiều Set-Cookie), skip ID3v2 tag ~18KB,
  crash do SetOutputVolume ghi NVS từ task PSRAM (chuyển sang Application::Schedule).
- File mới: `music_ui.h/.cc` (UI phát nhạc: ảnh bìa trái + tên/ca sĩ phải),
  `zing_client.*`, `zing_music_player.*` (đã có từ trước, cập nhật).
- Auto-play khi boot đã TẮT (`ZING_MUSIC_BOOT_TEST 0` trong board .cc).
