#!/usr/bin/env bash
# 麒麟／UOS tar.gz 测试：无 Python，支持自动／强制软件渲染。
set -uo pipefail

usage() {
    cat <<'EOF'
用法：bash test-shotlet.sh [Shotlet.tar.gz] [--check-only|--prepare-only]
                           [--software] [--output-dir 新目录] [--runtime-dir 固定运行目录]

默认自动选择本机架构的包，解压到脚本旁固定目录；重跑会校验并复用程序。
--check-only       检查安装包、架构、系统库和启动，不打开截图界面。
--prepare-only     只校验、解压并打印程序路径，不执行程序，适合先办理 KYSEC 授权。
--software         强制 CPU 软件渲染，适合缺少可用 Vulkan 设备的桌面。
--output-dir DIR   日志／截图结果目录，须不存在或为空，默认 /tmp/shotlet-test.*。
--runtime-dir DIR  程序目录，须为空或包含同一安装包的完整程序，不覆盖已存在程序。
EOF
}

archive=""; output_dir=""; runtime_dir=""
check_only=0; prepare_only=0
renderer_mode=${SHOTLET_RENDERER:-auto}
while (($#)); do
    case "$1" in
        --check-only) check_only=1; shift ;;
        --prepare-only) prepare_only=1; shift ;;
        --software) renderer_mode=software; shift ;;
        --output-dir|--runtime-dir)
            [[ $# -ge 2 && -n $2 ]] || { usage >&2; exit 1; }
            if [[ $1 == --output-dir ]]; then output_dir=$2; else runtime_dir=$2; fi
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        --*) printf '未知参数：%s\n' "$1" >&2; exit 1 ;;
        *)
            [[ -z $archive ]] || { printf '一次只能测试一个包。\n' >&2; exit 1; }
            archive=$1; shift ;;
    esac
done
((check_only + prepare_only <= 1)) || { printf '预检与仅准备模式不能同时使用。\n' >&2; exit 1; }
case "$renderer_mode" in auto|software|vulkan) ;; *) printf 'SHOTLET_RENDERER 应为 auto、software 或 vulkan。\n' >&2; exit 1 ;; esac
[[ $(uname -s) == Linux ]] || { printf '请在 Linux 运行。\n' >&2; exit 1; }
for tool in tar ldd getconf tee mktemp sha256sum od stat grep; do
    command -v "$tool" >/dev/null 2>&1 || { printf '缺少命令：%s\n' "$tool" >&2; exit 1; }
done
case $(uname -m) in
    x86_64|amd64) package_arch=amd64; elf_machine=62 ;;
    aarch64|arm64) package_arch=arm64; elf_machine=183 ;;
    *) printf '当前仅支持 x86_64 和 ARM64。\n' >&2; exit 1 ;;
esac
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || exit 1
if [[ -z $archive ]]; then
    shopt -s nullglob
    candidates=("$script_dir"/shotlet-*-linux-"$package_arch".tar.gz)
    ((${#candidates[@]})) || candidates=("$PWD"/shotlet-*-linux-"$package_arch".tar.gz)
    ((${#candidates[@]} == 1)) || { printf '请显式指定本机架构的 tar.gz 路径。\n' >&2; exit 1; }
    archive=${candidates[0]}
fi
[[ -f $archive ]] || { printf '安装包不存在：%s\n' "$archive" >&2; exit 1; }
archive=$(cd -- "$(dirname -- "$archive")" && printf '%s/%s' "$PWD" "$(basename -- "$archive")") || exit 1
umask 077
if [[ -n $output_dir ]]; then
    if [[ -e $output_dir ]] && [[ ! -d $output_dir || -n $(ls -A -- "$output_dir") ]]; then
        printf '结果目录必须不存在或为空：%s\n' "$output_dir" >&2; exit 1
    fi
    mkdir -p -- "$output_dir" || exit 1
    work_dir=$(cd -- "$output_dir" && pwd -P) || exit 1
else
    work_dir=$(mktemp -d "${TMPDIR:-/tmp}/shotlet-test.XXXXXX") || exit 1
fi
exec > >(tee "$work_dir/report.txt") 2>&1
trap 'printf "\n结果保留在：%s\n" "$work_dir"' EXIT
fail() { printf '[FAIL] %s\n' "$*" >&2; exit 1; }
execution_failure() {
    local log=$1 stage=$2
    tail -n 40 "$log"
    if grep -Eqi 'kysec|permission denied|权限不足|权限不够|拒绝访问' "$log"; then
        printf '[BLOCKED] %s被系统拒绝，尚未验证图形功能。\n程序：%s\n' "$stage" "$binary"
        printf '请检查该文件的执行权限、挂载限制和安全策略。\n'
        printf '若由麒麟 KYSEC 拦截，请按单位策略由管理员在“安全中心→应用防护→应用程序执行控制→高级配置”中仅授权上面的程序文件，再重跑。\n'
        printf '脚本不会关闭安全控制，也不会重新覆盖已经校验通过的程序。\n'
        exit 3
    fi
    fail "$stage失败，详见：$log"
}
printf 'Shotlet tar.gz 测试（无 Python）\n时间：%s\n安装包：%s\n结果目录：%s\n渲染模式：%s\n' "$(date -Is)" "$archive" "$work_dir" "$renderer_mode"
uname -a
if [[ -r /etc/os-release ]]; then
    while IFS= read -r line; do
        case "$line" in PRETTY_NAME=*|ID=*|VERSION_ID=*) printf '%s\n' "$line" ;; esac
    done < /etc/os-release
fi
printf '桌面：%s；会话：%s；DISPLAY=%s\n' "${XDG_CURRENT_DESKTOP:-unknown}" "${XDG_SESSION_TYPE:-unknown}" "${DISPLAY:-unset}"
glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null) || fail 'getconf 无法读取 GNU glibc 版本；不是版本过低的判断结果。'
printf '运行库：%s\n' "$glibc"
if [[ $glibc =~ ^glibc[[:space:]]+([0-9]+)\.([0-9]+)(\.[0-9]+)*$ ]]; then
    glibc_major=$((10#${BASH_REMATCH[1]})); glibc_minor=$((10#${BASH_REMATCH[2]}))
else
    fail '无法解析 glibc 版本文本。'
fi
((glibc_major > 2 || (glibc_major == 2 && glibc_minor >= 28))) || fail '实际 glibc 版本低于 2.28。'
printf '[PASS] %s 满足最低版本 2.28。\n' "$glibc"

archive_name=$(basename -- "$archive")
archive_sha_output=$(sha256sum -- "$archive") || fail '无法读取安装包进行 SHA256 检查。'
archive_sha=${archive_sha_output%% *}
expected=""
if [[ -r $(dirname -- "$archive")/SHA256SUMS ]]; then
    while read -r checksum filename; do
        if [[ ${filename#\*} == "$archive_name" && $checksum =~ ^[0-9a-fA-F]{64}$ ]]; then
            expected=${checksum,,}; break
        fi
    done < "$(dirname -- "$archive")/SHA256SUMS"
fi
if [[ -n $expected ]]; then
    [[ $archive_sha == "$expected" ]] || fail 'SHA256 校验失败，请重新复制安装包。'
    printf '[PASS] SHA256 校验通过。\n'
else
    printf '[WARN] 没有匹配的 SHA256SUMS，未校验包来源清单。\n'
fi

tar --quoting-style=escape -tzf "$archive" > "$work_dir/members.txt" 2> "$work_dir/archive.log" || fail '无法读取 tar.gz，详见 archive.log。'
tar --quoting-style=escape -tvzf "$archive" > "$work_dir/member-types.txt" 2>> "$work_dir/archive.log" || fail '无法读取 tar 文件类型。'
binary_members=()
while IFS= read -r member; do
    [[ $member =~ ^[a-zA-Z0-9._/-]+$ ]] || fail "不支持的包内路径：$member"
    case "$member" in /*|..|../*|*/../*|*/..) fail "不安全的包内路径：$member" ;; esac
    case "$member" in usr/bin/shotlet|*/usr/bin/shotlet) binary_members+=("$member") ;; esac
done < "$work_dir/members.txt"
total_bytes=0
while read -r mode owner size rest; do
    case ${mode:0:1} in -|d) ;; *) fail '包内含链接或特殊文件，拒绝解压。' ;; esac
    [[ $size =~ ^[0-9]+$ && ${#size} -le 9 ]] || fail '无法识别包内文件大小。'
    ((10#$size <= 536870912)) || fail '安装包解压体积异常。'
    total_bytes=$((total_bytes + 10#$size))
    ((total_bytes <= 536870912)) || fail '安装包解压体积超过 512 MiB。'
done < "$work_dir/member-types.txt"
((${#binary_members[@]} == 1)) || fail '未找到唯一的 usr/bin/shotlet。'
binary_member=${binary_members[0]}
payload_sha_output=$(tar -xOzf "$archive" -- "$binary_member" | sha256sum) || fail '无法读取包内程序。'
payload_sha=${payload_sha_output%% *}
if [[ -z $runtime_dir ]]; then
    runtime_dir="$script_dir/shotlet-runtime-$package_arch-${archive_sha:0:12}"
fi
[[ ! -L $runtime_dir ]] || fail '运行目录不能是符号链接。'
extract_runtime=0
if [[ ! -e $runtime_dir ]]; then
    mkdir -p -- "$runtime_dir" || fail '无法创建固定运行目录，请用 --runtime-dir 指定可写位置。'
    extract_runtime=1
elif [[ ! -d $runtime_dir ]]; then
    fail '运行目录不是目录。'
elif [[ -z $(ls -A -- "$runtime_dir") ]]; then
    extract_runtime=1
fi
runtime_dir=$(cd -- "$runtime_dir" && pwd -P) || fail '无法进入运行目录。'
if ((extract_runtime)); then
    tar -xzf "$archive" --no-same-owner --no-same-permissions -C "$runtime_dir" >> "$work_dir/archive.log" 2>&1 || fail '解压失败，请检查 archive.log，并换一个新的 --runtime-dir。'
    printf '[PASS] 已解压到固定目录：%s\n' "$runtime_dir"
else
    printf '[INFO] 复用已有运行目录，不重新写入程序：%s\n' "$runtime_dir"
fi
binary="$runtime_dir/$binary_member"
[[ -f $binary && ! -L $binary && -x $binary ]] || fail '运行目录缺少可执行的 Shotlet，请换一个新的 --runtime-dir。'
actual_sha_output=$(sha256sum -- "$binary") || fail '无法读取已解压程序。'
[[ ${actual_sha_output%% *} == "$payload_sha" ]] || fail '已有程序与安装包不匹配；不会覆盖，请换一个新的 --runtime-dir。'
read -r -a elf_header <<< "$(od -An -v -t u1 -N 20 -w20 -- "$binary")"
[[ ${#elf_header[@]} == 20 ]] || fail 'ELF 头不完整。'
[[ ${elf_header[*]:0:6} == '127 69 76 70 2 1' ]] || fail '需要小端 ELF64 程序。'
((elf_header[18] + 256 * elf_header[19] == elf_machine)) || fail '安装包 CPU 架构与本机不匹配。'
printf '[PASS] 程序完整性及 ELF 架构检查通过。\n程序：%s\n' "$binary"
if ((prepare_only)); then
    printf '[READY] 只准备程序，未执行。若需 KYSEC 授权，请管理员仅授权上面的文件，再用同一运行目录重跑。\n'
    exit 0
fi
if ! ldd "$binary" > "$work_dir/dependencies.txt" 2>&1; then
    execution_failure "$work_dir/dependencies.txt" '动态库检查'
fi
cat "$work_dir/dependencies.txt"
grep -q 'not found' "$work_dir/dependencies.txt" && fail '缺少上面列出的系统库。'
if ! "$binary" --version > "$work_dir/startup.txt" 2>&1; then
    execution_failure "$work_dir/startup.txt" 'Shotlet 启动自检'
fi
cat "$work_dir/startup.txt"
printf '[PASS] Shotlet 启动自检通过。\n'
ldconfig_command=$(command -v ldconfig || true)
[[ -n $ldconfig_command ]] || { [[ ! -x /sbin/ldconfig ]] || ldconfig_command=/sbin/ldconfig; }
if [[ -n $ldconfig_command ]]; then
    "$ldconfig_command" -p > "$work_dir/loader-cache.txt" 2>&1 || true
    if grep 'libvulkan\.so\.1' "$work_dir/loader-cache.txt"; then
        printf '[INFO] 系统缓存列出了 Vulkan 加载器，实际 GPU 功能仍由图形测试验证。\n'
    else
        printf '[INFO] 缓存未列出 Vulkan 加载器；此 Linux 构建的自动模式可回退 CPU 软件渲染。\n'
    fi
fi
if command -v fc-match >/dev/null 2>&1; then
    printf '中文匹配字体：'
    fc-match -f '%{family} | %{file}\n' 'sans-serif:lang=zh-cn'
fi
if ((check_only)); then
    printf '[PASS] 基础预检通过；未测试桌面、GPU、标注或保存。\n'
    exit 0
fi
[[ -n ${DISPLAY:-} ]] || fail '没有 DISPLAY，请在本机图形桌面的终端运行。'
[[ -z ${WAYLAND_DISPLAY:-} && ${XDG_SESSION_TYPE:-} != wayland ]] || fail '当前为 Wayland，请切换到 X11。'
command -v zenity >/dev/null 2>&1 || fail '缺少 zenity；DEB 桌面可执行 sudo apt install zenity。'
if [[ $renderer_mode != software ]] && command -v vulkaninfo >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1; then
    timeout 15s vulkaninfo --summary > "$work_dir/vulkan.txt" 2>&1 || printf '[WARN] vulkaninfo 未成功，详见 vulkan.txt。\n'
fi
cat <<EOF

请拉选区、添加矩形／箭头和中文，再点击“完成”、保存或 Ctrl+S。
请保存到：$work_dir/Screenshot.png
必要时在保存对话框按 Ctrl+L 输入上述路径。
应用日志：$work_dir/app.log
EOF
if (
    cd -- "$work_dir" || exit 1
    env SDL_FILE_DIALOG_DRIVER=zenity SHOTLET_RENDERER="$renderer_mode" \
        XDG_CONFIG_HOME="$work_dir/config" XDG_DATA_HOME="$work_dir/data" \
        XDG_STATE_HOME="$work_dir/state" XDG_CACHE_HOME="$work_dir/cache" \
        "$binary" --language zh-CN --log-level debug --log-dir "$work_dir/logs"
) > "$work_dir/app.log" 2>&1; then
    app_status=0
else
    app_status=$?
fi
if [[ $app_status == 2 ]]; then
    printf '[CANCEL] 用户取消，图形验收未完成。\n'; exit 2
elif [[ $app_status != 0 ]]; then
    execution_failure "$work_dir/app.log" '图形启动或保存'
fi
grep -E 'event=renderer.selected|event=overlay.renderer|event=capture.output_rendered' "$work_dir/app.log" || true

verify_png() {
    local image=$1 bytes width height position=8 size end chunks=0 has_pixels=0 ended=0
    local -a header chunk footer
    bytes=$(stat -c %s -- "$image") || return 1
    ((bytes >= 57 && bytes <= 536870912)) || return 1
    read -r -a header <<< "$(od -An -v -t u1 -N 24 -w24 -- "$image")"
    [[ ${#header[@]} == 24 && ${header[*]:0:8} == '137 80 78 71 13 10 26 10' ]] || return 1
    [[ ${header[*]:8:8} == '0 0 0 13 73 72 68 82' ]] || return 1
    width=$(((header[16] << 24) + (header[17] << 16) + (header[18] << 8) + header[19]))
    height=$(((header[20] << 24) + (header[21] << 16) + (header[22] << 8) + header[23]))
    ((width > 0 && height > 0 && width <= 65535 && height <= 65535)) || return 1
    while ((position + 12 <= bytes && chunks < 100000)); do
        read -r -a chunk <<< "$(od -An -v -t u1 -j "$position" -N 8 -w8 -- "$image")"
        [[ ${#chunk[@]} == 8 ]] || return 1
        size=$(((chunk[0] << 24) + (chunk[1] << 16) + (chunk[2] << 8) + chunk[3]))
        end=$((position + 12 + size))
        ((end <= bytes)) || return 1
        if [[ ${chunk[*]:4:4} == '73 68 65 84' ]] && ((size > 0)); then has_pixels=1; fi
        if [[ ${chunk[*]:4:4} == '73 69 78 68' ]]; then
            ((size == 0 && end == bytes)) || return 1
            read -r -a footer <<< "$(od -An -v -t u1 -j "$position" -N 12 -w12 -- "$image")"
            [[ ${footer[*]} == '0 0 0 0 73 69 78 68 174 66 96 130' ]] || return 1
            ended=1; break
        fi
        position=$end; chunks=$((chunks + 1))
    done
    ((ended && has_pixels)) || return 1
    printf '[PASS] PNG 签名、尺寸、分块和 IEND 检查通过：%s（%sx%s）。\n' "$image" "$width" "$height"
    if command -v pngcheck >/dev/null 2>&1; then
        pngcheck -q "$image" || return 1
        printf '[PASS] pngcheck 校验通过。\n'
    else
        printf '[INFO] 未执行 PNG 数据 CRC／解码校验，仍需打开图片目视确认。\n'
    fi
    sha256sum -- "$image"
}
shopt -s nullglob nocaseglob
images=("$work_dir"/*.png)
((${#images[@]})) || fail '测试目录没有 PNG，请按提示位置保存。'
for image in "${images[@]}"; do
    verify_png "$image" || fail "PNG 结构或内容检查失败：$image"
done
printf '[待目视确认] 请打开 PNG，检查选区、标注和中文显示是否符合预期。\n'
printf '[PASS] 截图应用正常完成，PNG 保存与结构检查通过。\n'
