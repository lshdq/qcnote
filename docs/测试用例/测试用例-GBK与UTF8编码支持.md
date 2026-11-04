# 测试用例：GBK 与 UTF-8 编码支持

## 测试范围

- 编码自动识别（EncodingDetector）：BOM / 严格 UTF-8 校验 / GBK 回退。
- 存储层（LocalStorage）：双编码读写、强制编码读取、BOM 写回、GBK 可编码性校验、原子写。
- 文档模型（Document）：encoding/hasBom 属性、applyLoadedState。
- 管理器（DocumentManager）：自动识别打开、reloadWithEncoding、saveWithEncoding（编码转换）、未命名文档边界。
- 会话（SessionManager）：编码持久化与恢复、旧会话兼容。
- QML 界面：菜单入口状态、状态栏显示、未保存确认框（offscreen）。
- 回归：既有 UTF-8 行为不变。

## 用例列表

### TC-001: 自动识别—UTF-8 BOM
- 前置条件：临时文件内容为 EF BB BF + UTF-8 中文文本。
- 操作步骤：调用 EncodingDetector::detect；再经 DocumentManager::openUrls 打开。
- 预期结果：识别为 UTF-8、hasBom=true；文档内容不含 BOM 字符，状态栏显示 "UTF-8 BOM"。
- 优先级：P0

### TC-002: 自动识别—无 BOM UTF-8（含中文）
- 前置条件：临时文件为合法 UTF-8 多字节序列（中文 + emoji），无 BOM。
- 操作步骤：detect + openUrls。
- 预期结果：UTF-8、hasBom=false，内容正确。
- 优先级：P0

### TC-003: 自动识别—纯 ASCII
- 前置条件：文件仅含 ASCII 字节。
- 操作步骤：detect。
- 预期结果：UTF-8（ASCII 为 UTF-8 子集），hasBom=false。
- 优先级：P0

### TC-004: 自动识别—GBK 中文
- 前置条件：临时文件为中文文本的 GBK 编码字节（如“你好”= C4 E3 BA C3）。
- 操作步骤：detect + openUrls。
- 预期结果：识别为 GBK；解码后内容与原文一致，无替换字符。
- 优先级：P0

### TC-005: 自动识别—无效序列回退 GBK
- 前置条件：文件含非法 UTF-8 字节序列（如 0x81 开头连续高位字节）。
- 操作步骤：detect。
- 预期结果：UTF-8 严格校验失败 → 判为 GBK；GB18030 无法解码的字节呈现为 U+FFFD，文件仍可打开。
- 优先级：P1

### TC-006: 读取—强制编码重载
- 前置条件：GBK 中文文件已按自动识别（GBK）打开。
- 操作步骤：LocalStorage::read(url, Encoding::Utf8, ...) 强制以 UTF-8 读取。
- 预期结果：返回替换字符呈现的乱码内容（不崩溃），接口正常返回。
- 优先级：P1

### TC-007: 写入—UTF-8 无 BOM 往返
- 前置条件：UTF-8 无 BOM 文档。
- 操作步骤：write(encoding=Utf8, writeBom=false) 后重读字节。
- 预期结果：磁盘无 BOM 字节，内容往返一致；既有行为不变。
- 优先级：P0

### TC-008: 写入—UTF-8 BOM 保留写回
- 前置条件：带 BOM 的 UTF-8 文件读取后修改内容。
- 操作步骤：write(encoding=Utf8, writeBom=true) 后检查磁盘字节。
- 预期结果：磁盘以 EF BB BF 开头，BOM 后为合法 UTF-8。
- 优先级：P0

### TC-009: 写入—GBK 往返
- 前置条件：中文 + 英文混合内容。
- 操作步骤：write(encoding=Gbk) 后以 GBK 强制读取。
- 预期结果：磁盘字节为 GBK（CP936）编码（无 BOM 前缀），往返内容一致。
- 优先级：P0

### TC-010: 写入—GBK 不可编码字符阻止保存
- 前置条件：文档内容含 emoji（如 😀）或 GBK 外生僻字。
- 操作步骤：write(encoding=Gbk)。
- 预期结果：返回 false，错误消息含该字符（及行号定位）；磁盘文件未被改动。
- 优先级：P0

### TC-011: Document 编码属性与 applyLoadedState
- 前置条件：Document 实例。
- 操作步骤：设置/读取 encoding、hasBom；applyLoadedState 带编码参数后检查 modified=false。
- 预期结果：属性读写、信号发射、不置脏符合预期；新文档默认 Utf8/无 BOM。
- 优先级：P0

### TC-012: reloadWithEncoding—纠正识别错误
- 前置条件：GBK 文件被误判为 UTF-8 打开（内容乱码、modified=false），或直接以 UTF-8 强制打开 GBK 文件。
- 操作步骤：DocumentManager::reloadWithEncoding(doc, Gbk)。
- 预期结果：内容变为正确中文；doc->encoding()==Gbk；modified 仍为 false；光标钳制到新内容长度内；换行风格重探测。
- 优先级：P0

### TC-013: reloadWithEncoding—未命名文档防御
- 前置条件：未命名文档（url 为空）。
- 操作步骤：reloadWithEncoding(doc, Gbk)。
- 预期结果：不崩溃，发 openFailed 提示（UI 层本就禁用入口，此处为 C++ 防御）。
- 优先级：P1

### TC-014: saveWithEncoding—GBK→UTF-8 转换
- 前置条件：GBK 中文文件已正确打开（encoding=Gbk）。
- 操作步骤：saveWithEncoding(doc, Utf8)。
- 预期结果：返回 true；磁盘字节为合法 UTF-8（BOM 状态按记录）；doc->encoding()==Utf8、modified==false。
- 优先级：P0

### TC-015: saveWithEncoding—UTF-8→GBK 转换
- 前置条件：UTF-8 中文文件已打开。
- 操作步骤：saveWithEncoding(doc, Gbk)。
- 预期结果：磁盘字节为 GBK 编码；内容含 GBK 外字符时按 TC-010 失败。
- 优先级：P0

### TC-016: 会话—编码记忆与恢复
- 前置条件：打开 GBK 文件并手动确认编码；SessionManager 正常写盘。
- 操作步骤：重建 DocumentManager/SessionManager 并 restoreDocuments。
- 预期结果：文档按记忆的编码打开（doc->encoding() 一致）；无编码记录的旧会话文件正常按自动识别恢复。
- 优先级：P0

### TC-017: UI—菜单入口与状态栏
- 前置条件：offscreen 启动，打开 GBK 文件。
- 操作步骤：检查主菜单「编码」子菜单项存在性与启用状态；状态栏文本。
- 预期结果：状态栏显示 "GBK"；「通过编码重新加载/保存」当前项打勾；未命名文档时重新加载项禁用。
- 优先级：P1

### TC-018: UI—未保存修改确认框
- 前置条件：打开文件并修改未保存。
- 操作步骤：菜单触发「通过编码重新加载」。
- 预期结果：弹出确认框（丢弃修改）；确认后重载，取消则内容不变。
- 优先级：P1

### TC-019: 回归—既有 UTF-8 行为
- 前置条件：master 既有测试集。
- 操作步骤：运行 tests/test_core、test_editor、test_window 全部测试。
- 预期结果：全部通过（接口签名变更同步修正后无行为回归）。
- 优先级：P0

## 测试命令

```powershell
$env:PATH = "D:\Program\Qt\Tools\CMake_64\bin;D:\Program\Qt\Tools\Ninja;" + $env:PATH
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=D:/Program/Qt/6.11.2/mingw_64 -DCMAKE_C_COMPILER=D:/Program/Qt/Tools/mingw1310_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=D:/Program/Qt/Tools/mingw1310_64/bin/g++.exe
cmake --build build
ctest --test-dir build --output-on-failure
```

冒烟（offscreen）：

```powershell
$env:PATH = "D:\Program\Qt\6.11.2\mingw_64\bin;D:\Program\Qt\Tools\mingw1310_64\bin;" + $env:PATH
$env:QT_QPA_PLATFORM = "offscreen"
# 启动 build/qcnote.exe 验证后 Get-Process qcnote | Stop-Process -Force
```

## 测试场景分类

- 通用（C++ 核心单元测试为主）+ 框架代码（Qt/QML 界面测试）
