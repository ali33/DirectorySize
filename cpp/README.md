<!-- DirectorySize (C++) — Copyright (c) 2026 META.vn · Author: Nguyễn Đức Sơn · SPDX-License-Identifier: GPL-2.0-only -->
# DirectorySize (C++)

[![cpp](https://github.com/ali33/DirectorySize/actions/workflows/cpp.yml/badge.svg)](https://github.com/ali33/DirectorySize/actions/workflows/cpp.yml)

Bản viết lại bằng C++17 của [DirectorySize (C#)](https://github.com/ali33/DirectorySize), cùng tác giả:
tìm các thư mục lớn nhất trong một thư mục. Có hai ứng dụng trên Windows (console và giao diện Win32 thuần)
và bản console cho Linux. Mã C# gốc không bị đụng tới.

- `dirsize-cli` — Windows và Linux. `dirsize-cli --help`, `--version`.
- `dirsize-gui` — Windows (Win32 thuần, ListView, thanh trạng thái, About trong menu Trợ giúp).

## Build

Windows (MinGW-w64 + Ninja; tệp exe liên kết tĩnh, không cần DLL MinGW):

```
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure
```

Linux (trong Docker, không cần cài gì lên máy; mã nguồn được mount chỉ-đọc, build nằm trong container):

```
sh cpp/scripts/test-linux.sh          # build + ctest
sh cpp/scripts/test-linux.sh tsan     # build với -fsanitize=thread + ctest
sh cpp/scripts/test-linux.sh bench    # quét /usr, so với find và du
sh cpp/scripts/test-linux.sh sigint   # Ctrl+C (SIGINT) dừng sạch, mã thoát 130
```

Từ PowerShell: `bash cpp/scripts/test-linux.sh` (Git Bash) hoặc
`docker build -f cpp/docker/Dockerfile.linux -t dirsize-linux-build cpp/docker` rồi
`docker run --rm -v ${PWD}/cpp:/src:ro dirsize-linux-build bash /src/scripts/container-build.sh test`.

Linux không có Docker: `cmake -S cpp -B build -G Ninja && cmake --build build && ctest --test-dir build`.

## Dùng

```
dirsize-cli [đường_dẫn] [-t N] [-j N] [-i MS] [-x] [--api find|nt] [-q]
```

| Tuỳ chọn | Ý nghĩa |
|---|---|
| `-t, --top N` | số thư mục lớn nhất cần in (mặc định 20) |
| `-j, --threads N` | số luồng quét (mặc định: số nhân, tối đa 16) |
| `-x, --one-file-system` | Linux: không vượt sang mount khác (như `du -x`) |
| `--api find\|nt` | Windows: `FindFirstFileExW` hoặc `NtQueryDirectoryFile` (mặc định `nt`) |
| `--hardlinks once\|all` | tệp nhiều hard link: tính một lần (mặc định Linux) hay mọi lần (mặc định Windows) |
| `-q` | không in tiến độ |

Ctrl+C (Windows: `SetConsoleCtrlHandler`; Linux: `SIGINT`/`SIGTERM`) dừng quét, in kết quả một phần, mã thoát 130.
Mã thoát: 0 xong, 1 lỗi, 2 sai tham số. Đầu ra là UTF-8 (Windows: đặt code page console 65001; nên dùng
Windows Terminal hoặc font có dấu tiếng Việt).

Giao diện: nhập hoặc chọn thư mục (hộp thoại `IFileOpenDialog`), bấm **Tính**, theo dõi top 100, bấm **Dừng**
để dừng. Thanh tiến độ chạy (marquee) suốt lúc quét; thanh trạng thái hiện số thư mục / tệp / dung lượng và
số thư mục không truy cập được (bấm vào để xem mẫu lỗi). Lỗi chọn thư mục hiện ra hộp thoại.

Chuột phải (hoặc phím Menu / Shift+F10) vào một dòng: **Copy đường dẫn** (mặc định), **Copy đường dẫn + dung lượng**
(phân tách bằng Tab), **Mở trong Explorer**, **Copy tất cả các dòng đang hiện**. Ctrl+C copy đường dẫn các dòng
đang chọn (chọn nhiều dòng được), mỗi dòng một đường dẫn. Danh sách được làm mới mỗi 500 ms nên khi menu mở danh
sách bị đóng băng và lựa chọn được giữ theo đường dẫn, không theo số thứ tự dòng. Thanh trạng thái báo "Đã copy: ..."
vài giây, hoặc lỗi nếu clipboard bị ứng dụng khác giữ.

## Biểu tượng

Một biểu tượng dùng chung cho C++ và C#: `assets/icon/dirsize.svg` (bản 16/24 px riêng: `dirsize-small.svg`),
xuất ra `dirsize.ico` (16-256) và `png/` bằng `sh assets/icon/build-icons.sh` (rsvg-convert + ImageMagick trong Docker).
Được nhúng vào cả `dirsize-cli.exe` và `dirsize-gui.exe` qua `.rc`.

## Kiến trúc

```
cpp/
  core/       thuật toán thuần C++, không phụ thuộc OS
    size_tree      cây thư mục thread-safe, cộng dồn lúc lấy snapshot
    scanner        chạy walker ở luồng nền, cờ dừng atomic, snapshot top-N
    work_pool.h    hàng đợi thư mục + thread pool
    size_format    "12.34 GB"
  platform/   chỗ cắm theo hệ điều hành (walker.h là giao diện IDirWalker)
    win_walker.cpp    Windows: \\?\, FindFirstFileExW hoặc NtQueryDirectoryFile
    posix_walker.cpp  Linux: fdopendir/readdir/fstatat
  apps/cli    console (Windows + Linux)
  apps/gui    Win32 thuần (chỉ Windows)
  tests/      test_core, test_walker (chạy qua ctest)
  docker/ scripts/   build + test Linux trong Docker
```

Quyết định thiết kế (đều đã đo, xem số liệu trong báo cáo kèm commit):

- Mỗi worker chỉ cộng dung lượng tệp vào đúng nút thư mục đang quét (một phép cộng atomic mỗi thư mục);
  cộng dồn lên thư mục cha chỉ làm khi lấy snapshot, bằng một lượt duyệt ngược theo chỉ số
  (cha luôn có chỉ số nhỏ hơn con). Không khoá trong vòng nóng; đường dẫn đầy đủ chỉ ghép cho top-N hiển thị.
- Bản C# gốc có lỗi: báo "Finished" khi mới duyệt xong cây mà hàng đợi tệp chưa được cộng hết (thiếu số),
  `Thread.Abort`, đọc dictionary ngoài khoá, và nuốt lỗi quyền truy cập. Bản C++ không có các lỗi này:
  thư mục không đọc được được đếm và hiện ra (CLI in mẫu lỗi, GUI hiện trên thanh trạng thái).
- Kích thước là kích thước logic (`EndOfFile` / `st_size`), không phải dung lượng cấp phát (`st_blocks`).
- Không đi theo symlink, junction, mount point (Windows: thư mục reparse point). Linux: symlink được tính
  bằng kích thước của chính nó (như `du --apparent-size`), và đếm vào "liên kết bỏ qua".
- Số "thư mục" ở đây là mọi thư mục đã gặp; bản C# chỉ đếm thư mục có chứa tệp.
- Windows: chỉ bỏ qua symlink và junction/mount point; reparse point loại khác (OneDrive...) vẫn được quét.
- Hard link (`--hardlinks once|all`): Linux mặc định tính một lần theo (st_dev, st_ino), chỉ tốn khi st_nlink > 1.
  Windows mặc định tính mọi lần vì tính một lần phải lấy file id của mọi tệp: đo trên `C:\Windows\WinSxS`
  (638 nghìn thư mục, 163 nghìn tệp) chậm hơn khoảng 12% (94 s so với 105 s, mỗi chế độ một lần đo, máy nhiễu);
  trên `System32` không thấy khác biệt. Tệp có nhiều link được tính ở thư mục nào gặp trước.
- Linux mở thư mục bằng `openat(dirfd, tên, O_NOFOLLOW)` nên không bị giới hạn `PATH_MAX` và không đi theo
  symlink được tráo vào giữa chừng (có test với đường dẫn dài hơn 4096 ký tự). Windows dùng tiền tố `\\?\`
  (và `\\.\` giữ nguyên) nên đường dẫn dài hơn 260 ký tự quét được (có test).
- Kết quả "đã dừng" chỉ khi việc quét thực sự bị cắt giữa chừng; bấm dừng đúng lúc quét vừa xong vẫn là "hoàn tất".
- Snapshot top-N được lấy lại từ bộ đệm khi cây không đổi và dùng lại bộ nhớ làm việc, nên cập nhật mỗi 500 ms
  không cấp phát theo số thư mục.

## Ghi chú toolchain

MSYS2 g++ 15.2 (MinGW-w64, thread model posix): một chương trình thử 16 luồng dùng `thread_local std::vector` /
`std::string` (có hàm huỷ) bị segfault 30/30 lần chạy ở cả ba kiểu liên kết: `-static`, `-static` kèm
`-Wl,--whole-archive -lwinpthread -Wl,--no-whole-archive`, và liên kết động. `thread_local` kiểu trivial (mảng, số
nguyên) chạy 30/30 lần ổn. Vì vậy mã không dùng `thread_local` có hàm huỷ (bộ đệm 64 KB nằm trên stack của worker).
Cờ whole-archive không khôi phục được `thread_local` an toàn; nguyên nhân gốc chưa tìm ra.

## Linux

Bản CLI Linux đã cài đặt và kiểm chứng trong Docker (Ubuntu 24.04, g++ 13): ctest qua, ThreadSanitizer qua,
tổng khớp từng byte với `find /usr \( -type f -o -type l \) -printf '%s'`. Chưa làm: bản giao diện Linux
(GUI dùng Win32 nên không port; nếu cần sẽ là một ứng dụng mới trong `apps/` dùng GTK/Qt trên cùng `core/`),
`getdents64` trực tiếp, và `statx`.

## Phát hành

Đẩy tag dạng `v*` thì workflow `.github/workflows/cpp.yml` build Windows (MinGW) và Linux, chạy test rồi tạo
GitHub Release kèm `dirsize-windows-x64.zip` và `dirsize-linux-x64.tar.gz`:

```
git tag v1.0.0
git push origin v1.0.0
```

## Bản quyền / Giấy phép

Copyright © 2026 META.vn — Tác giả: Nguyễn Đức Sơn.

Giấy phép GNU GPL phiên bản 2 (GPL-2.0-only), như bản C# gốc. Toàn văn trong [LICENSE](LICENSE).
Phần mềm này KHÔNG có bảo hành nào (ABSOLUTELY NO WARRANTY).
