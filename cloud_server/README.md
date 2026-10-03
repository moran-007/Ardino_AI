# ESP32 云端语音服务

## 完整音乐播放器（多麦克风 2.1.0）

`/admin/music` 管理完整歌曲、同步歌词和全部用户歌单，管理员可查看用户私有歌曲。原始 MP3 / PCM WAV 保存在 `VOICE_DATA_DIR/music-library/<id>/source`；SQLite 保存元数据、LRC 时间轴、设备归属和歌单顺序。歌曲名、歌手依次参考用户填写、音频标签、LRC 标签和文件名；音频类型按内容识别，扩展名错写也可识别。歌词支持 UTF-8/BOM、UTF-16、GB18030，以及一行多时间标签、重复时间标签和 offset。

完整音频没有固定文件大小、时长或音乐数量上限，不截取成短片段；限制为实际磁盘容量、网络和服务器处理能力，写入时保留约 256 MB 磁盘余量，失败或取消清理临时文件。LRC 最多 1 MB，单句最多 256 字符，设备分批读取 100 句。上传采用增量 multipart 解析，64 KiB 块落盘，不将整首音乐载入服务器或设备内存。Nginx 仅 `/admin/music/upload` 与 `/voice-api/v1/music/browser-upload` 配置 `client_max_body_size 0` 和关闭请求缓存，其他接口保留原有限额。

设备网页先向已鉴权 ESP32 申请一次性上传凭据，再由浏览器直接上传云端。凭据 10 分钟内开始上传有效，绑定精确网页 Origin、设备和可选歌词目标；上传开始后不因大文件传输超过 10 分钟而到期。永久设备令牌不提供给浏览器。管理员上传需要会话与 `X-CSRF-Token`，服务器仅为合法上传来源提供 CORS。歌曲可全库搜索、分页读取，用户只能访问共用歌曲和自己的歌曲，并管理自己的歌单。

完整歌曲实时由 FFmpeg 解码为 **48 kHz / 16 位 / 单声道 PCM**，保留原始音乐，设备不额外进行有损转码。单只 MAX98357A / 喇叭使用单声道；设备按实际输出采样数推进歌词，网络缓冲停顿不会推进。PCM 采用未知长度连续流；音乐客户端使用 HTTP/1.0 请求和关闭复用，经代理移除 chunk framing，读到真正的连接结束。`offset_bytes` 是偶数、64 位 PCM 偏移，恢复时带解码预滚，时长字段仅作元数据估计。最多四路服务器解码，每个设备一条；断开或取消回收子进程，输出管道有界。语音及闹钟接口仍使用原 16 kHz。

设备令牌鉴权接口（前缀 `/voice-api/v1`）：

| 接口 | 用途 |
| --- | --- |
| `GET /music?q=&cursor=&limit=` / `GET /music/{id}` | 搜索、分页目录 / 单曲元数据 |
| `GET /music/{id}/pcm?offset_bytes=` | 连续 PCM 流，不缓存整首解码文件 |
| `GET /music/{id}/lyrics?cursor=&limit=` | 同步歌词分页 |
| `GET /music/queue?playlist_id=&current=&direction=next/previous` | 按歌单或目录顺序切曲并循环 |
| `GET/POST /music/playlists` | 读取 / 创建自己的歌单 |
| `GET/DELETE /music/playlists/{id}` | 分页读取 / 删除自己的歌单 |
| `POST /music/playlists/{id}/tracks` / `DELETE /music/playlists/{id}/tracks/{track_id}` | 添加 / 移除歌单歌曲 |
| `POST /music/upload-tickets` | 申请临时凭据，JSON `origin`、可选 `track_id` |
| `POST /music/browser-upload` | 浏览器增量 FormData：`audio`、可选 `lyrics/title/artist`，或 `track_id+lyrics` 补歌词 |
| `DELETE /music/{id}` | 删除自己的歌曲并移除歌单引用 |

完整播放器测试及人工验收见仓库 `docs/音乐播放器测试文档.md`。音乐文件本身不提交公开 GitHub。短铃声上传仍适用下面的独立限制。

## 音乐闹钟与两条固件通道（2026-10-02）

管理员页面 `/admin/alarm-music` 提供三首原创铃声和 MP3 / WAV 上传、试听、删除。共用库最多 12 首（含示例和各设备私有铃声）。MP3 源文件最多 20 MB，由 FFmpeg 截取前 30 秒并转为 16000 Hz / 16 位 / 单声道 PCM WAV；WAV 文件最多 1 MB、30 秒且须符合上述格式。服务器须安装 FFmpeg（Ubuntu：`apt-get install --no-install-recommends ffmpeg`），Nginx 的 `/admin/` 请求上限为 21 MB。转换单任务运行、超时 20 秒，拒绝损坏 MP3。

设备鉴权接口：`GET /voice-api/v1/alarm-music`、`GET/PUT /voice-api/v1/alarm-music/selection`（JSON `{ "id": "morning" }`）、`GET /voice-api/v1/alarm-music/{id}/pcm`、`POST /voice-api/v1/alarm-music-upload?title=名称`（原始 WAV，最多 512 KiB）。设备各自最多上传 6 首，私有铃声不能被其他设备下载或选用。音乐和选择保存在 `VOICE_DATA_DIR/alarm-music`，不混入语音用量计费。

ESP32 闹钟网页在浏览器解码、截取前 15 秒并转换，再通过设备临时文件分块转传；源文件最大 20 MB。这是独立短铃声功能，完整歌曲使用上面的完整音乐库。

OTA 清单和下载依据 `X-Hardware-ID` 分离：原单麦克风 `esp32s3-cloud-voice-v1` 存在 `firmware/`，多麦克风 `esp32s3-cloud-voice-multimic-v1` 存在 `firmware-multimic/`，分别保持当前发布指针。管理页同时显示两个通道；多麦克风 ZIP 上传需要表单 `channel=multi`，签名的硬件标识必须匹配。相同版本号的镜像也按硬件隔离。

费用页和设备用量接口从完整调用记录生成日期、用量、当时单价、总价格和跟随筛选范围的费用曲线（近 24 小时按小时、近 7/30 天按天、累计自动选择粒度）；币种分别统计，未计价不当作零元。设备响应只裁剪展示行和曲线范围，不裁剪累计合计。

这是与 `pc_server` 完全隔离的公网版本。当前第一版链路为：

```text
ESP32/电脑模拟器 -> 设备鉴权 -> ASR -> DeepSeek -> Qwen -> GLM
                                      -> 中英双 VITS 路由 -> 16 kHz PCM 分段
ESP32/电脑模拟器 <- 每生成一段就轮询、下载并立即播放
```

服务只在本机/服务器保存大模型厂商 Key。ESP32 只保存 Wi-Fi、`device_id` 和该设备自己的 Token。数据库只保存 Token 的 HMAC-SHA256 摘要；注册时的明文 Token 只显示一次。

## 已实现

- `POST /voice-api/v1/dialogue`：上传 16 kHz、16-bit、单声道裸 PCM。
- 设备可在对话请求的 `X-Device-Weather` 头中提交 URL 编码的当前城市、实况和三天预报；服务端限长、解码后作为只读数据交给 AI，用于回答当前地点天气问题。任务创建响应中的 `context.schema=1` 与 `context.weather=true` 是云端实际接收成功的确认，不应仅凭设备本地的天气刷新日志判定联动成功。
- 设备可在 `X-Device-Memos` 头中提交 URL 编码、限长后的便签/待办快照，使 AI 能回答列表查询；实际增删改仍由设备本地持久化执行。
- 新固件通过 `X-Device-Local-Control: 1` 协商执行回执：服务器先公开识别文字，等待设备以相同设备凭证 POST `/voice-api/v1/jobs/{job_id}/local-result` 提交 `{handled,message}`。操作成功、失败或部分成功的原始设备结果直接合成播报，任务来源为 `device_result`，不调用聊天模型。未匹配操作提交 `handled:false` 后继续正常对话；超过 8 秒未收到确认则本轮失败，绝不播报操作成功。同一任务不重复执行，重复回执不得覆盖不同结果。旧固件不等待新回执，兼容原有接口，但提示词不再凭空宣称操作完成。设备天气或便签快照不是执行成功凭证。
- `GET /voice-api/v1/jobs/{id}/segments?after=N`：按序获得已就绪的句级音频段。
- `GET /voice-api/v1/jobs/{id}/segments/{index}/audio`：下载长度确定的 16 kHz PCM，适配 ESP32 当前播放方式。
- `GET /voice-api/v1/jobs/{id}/audio`：任务完成后的合并音频兼容接口。
- 任务详情同时返回数据库字段 `question`/`answer` 和 ESP32 兼容字段 `recognized_text`/`answer_text`。
- `POST /voice-api/v1/translate`：使用相同 LLM 主备链路完成文字翻译。
- `GET/PUT /voice-api/v1/device-settings`：设备用自己的 Token 读取/修改中文音色、英文音色和语速。
- `GET /voice-api/v1/firmware/manifest`：按设备版本返回签名 OTA 清单；无更新返回 204。
- `GET /voice-api/v1/firmware/download/{version}`：凭设备 Token 下载当前发布固件。
- 每设备独立 Bearer Token、资源归属校验、每分钟限流、上传大小限制。
- DeepSeek、通义千问、GLM 支持自定义主备顺序；千问优先使用 DashScope 原生 HTTP/SSE，DeepSeek 和智谱直连各自官方 Chat Completions 接口，也保留自定义兼容地址。
- SenseVoice、本地中文 VITS 与 Piper 英文 VITS；混合文本按语种路由后统一为 ESP32 所需的 16 kHz PCM。
- 中文音色（0～4）、英文音色（0～903）和语速（0.5～2.0）按设备 ID 隔离，不修改服务器全局值。
- 无费用 mock Provider、电脑 ESP32 协议模拟器、自动测试。
- 单密码管理员页面：无用户名，支持接口开关、管理员密码修改、三家 LLM API、ASR/TTS Provider、本地模型路径、VITS 音色与语速；Key 加密保存且不回显。

## 管理员配置页

部署后的入口为：

```text
http://voice.bsnlch.xyz/admin/
```

输入唯一管理员密码即可解锁，不需要用户名。会话有效 8 小时，也可主动“锁定页面”。后台按用途拆成五页：

- `/admin/` 用量总览：累计、近 24 小时、7 天、30 天的 Token 总量、输入/输出、调用成功/失败及未返回用量次数；每个上游 API Key 独立卡片，设备汇总和最近 20 次调用。总览每 30 秒刷新。
- `/admin/devices` 设备 KEY：每台设备独立卡片，展示用量、最近使用、启停、角色、记忆、中英文音色和语速；创建设备后 Token 仍只显示一次。
- `/admin/api-keys` 语言模型配置：主用与两级备用可选中国厂商、自定义外部 API 或服务器本地语言模型。自定义／本地可选 OpenAI 兼容 SSE 或 Ollama 原生 NDJSON，地址支持端口及完整路径，Key 可选。默认不启用新接入，不自动安装或下载模型；任意私有 HTTP 协议需独立适配。保存后立即应用，Key 加密保存且不回显。
- `/admin/firmware` 固件版本：签名 OTA 上传、最低来源版本、更新说明、历史归档与下载。
- `/admin/settings` 服务设置：接口开关、管理员密码、本地 ASR/TTS、分段参数和推荐配置恢复。
- `/admin/billing` 计费与费用：按回传的输入、输出、缓存命中 Token 和已确认的搜索次数计价；默认官方规则，也可手动设置模型、币种、分档单价、缓存和搜索单价。统计开关默认开启，不会自动扣款。

### AI 回答与联网设置

`/admin/settings` 展示全局系统提示词和默认回答风格（简短、适中、详细）；`/admin/devices` 和设备网页“语音与音量 → AI 对话设置”可独立设置角色、回答风格、记忆轮数和联网搜索。联网搜索按设备保存，默认关闭，重启后保留。设备凭证通过 `GET/PUT /voice-api/v1/dialogue-settings` 读取和保存，不返回厂商 Key。

明确的本地天气、便签待办只读查询、北京时间日期和两数简单算术先用数据／规则直接回答，任务来源标记为 `local_rules`，不调用任何语言模型、不产生模型 Token 用量；仍保留分段播报和可选记忆。天气须匹配地点、缓存少于 90 分钟且覆盖所问日期；数据失效、跨城市、复合或不确定问题回到语言模型，不猜测、不用规则执行删除等操作。设备仍须连接服务器完成 ASR／TTS，不等于 ESP32 完全离线。

联网搜索默认关闭。开启后，确需查询的实时问题优先用已配置且支持搜索的千问；普通对话遵循主备顺序。不会复用或上传设备高德 Key。只有厂商确认搜索执行才播报搜索回答；失败时回退普通模型并如实说明无法确认实时数据。分段字数不限制总回答长度。

### 语音主备

`/admin/settings` 中识别与合成各自支持仅本地、本地主用云端备用、云端主用本地备用、仅云端，默认仅本地。云端备用只在主用异常或空／无效结果时调用；正常但错字的结果不会自动再次消费云端额度。独立语音 Key 加密保存，不自动复用聊天 Key。云端采用百炼官方 DashScope SDK 的 WSS 协议，与 OpenAI 兼容聊天接口不同。

16k 识别支持 Paraformer realtime v1/v2 和 Qwen Audio 3.0/3.1 ASR streaming；合成支持 Sambert 发音人模型，返回与播放采样率一致的 PCM。识别模型 ID 可手填，合成音色按男声、女声与童声、其他语言分组选择，也可手填覆盖。列表仅为建议，不代表账户权限或余额。模型名不再固定白名单，但不能靠改名改变协议：文件转写、8k、翻译、CosyVoice 与 Qwen TTS 在当前适配中会明确提示接口或参数不匹配，不伪装成已支持。临时识别音频在调用结束后删除，不增加诊断录音保留数。未配置或未授权的模型不会被自动试用。

服务设置的“小样本连通与试听”使用填写值测试，空 Key 复用加密保存的语音 Key。管理员鉴权、CSRF 和费用确认必需；一次只测 ASR 或 TTS，固定短句“今天天气怎么样”，ASR 输入由真实本地 TTS 生成（Mock 提示音不用于识别测试）。不保存测试填写值，不修改生产主备顺序，不调用聊天模型，不覆盖设备诊断录音。每次真实云端尝试归入 `admin-connectivity-test` 语音用量；不匹配协议或本地样本失败不调用云端。全局串行、至少间隔 10 秒、设备任务运行时拒绝测试。结果重定向到 GET 页，刷新不重复收费；SDK 原始错误和 Key 不回显，仅显示安全的失败类别。试听 WAV 仅保留最新一条于进程内存，由当前管理员会话鉴权访问，5 分钟后不可访问，下次测试或进程重启清除。连通测试不是麦克风准确度验证，也不保证所有阿里云模型使用相同接口。

云端语音调用在费用页独立列出最近 30 次发送时长／字数和原价估算，支持手动发送量单价；不混入 LLM Token 汇总，不扣除免费额度。Qwen Audio 3.1 按 Token 计费，当前该语音适配器未提取其 Token 用量，明确标为未计价，不套用秒数价格。失败调用不按免费处理，费用待确认。账号实时额度、用完即停和月度重置由百炼控制台控制，本服务不推算剩余额度。

设备网页首页提供手动清空本机聊天记录，须勾选确认；可另选同时清空本设备的服务器记忆（`POST /voice-api/v1/memory/clear`）。不会删除照片、提醒、用量记录或其他设备数据。服务器任务进行中禁止清除记忆；清空记忆不等于删除服务器历史任务记录。

### 计费口径

官方价格快照核对日期为 2026-10-01，当前覆盖官方 DeepSeek Flash／V4 Pro、北京千问 Plus／Max／3.5 Plus／3.6 Plus 和智谱 GLM-4.7-Flash。DeepSeek 采用官方美元单价，按请求开始时间计算峰谷，2026 年中国节假日已核对；未来年份高峰窗口缺少节假日规则时不自动猜测。千问使用非思考、实时调用价格，含上下文档位、隐式缓存及北京 turbo 搜索价格。第三方代理、其他地域、未知模型须手动设置，不按免费处理。

每次请求保留单价快照，以 Decimal 计算并保存十进制金额，CNY／USD 分开汇总。规则修改只影响新请求，旧用量保留为“历史未计价”，不回填现行单价。失败或主备重试也记录实际回传用量；未回传 Token 不计价，缺少缓存用量按未命中估算并标记；未确认搜索费用时只汇总已知模型费用并标记不完整。金额不含赠送额度、套餐、账户优惠或其他应用产生的调用，不等于厂商账单；价格不会自动从网页抓取刷新，请定期核对官方来源。

“获取模型列表”只使用已保存的 Key 和地址，不发起生成请求、不修改当前模型。DeepSeek 读取官方 [`GET /models`](https://api-docs.deepseek.com/zh-cn/api/list-models/)，千问读取原生 [`GET /api/v1/models`](https://help.aliyun.com/zh/model-studio/list-models) 并按文本生成能力分页；千问的新北京 WorkspaceId 域名需填写自己的完整地址。智谱未提供已确认的通用目录接口时，明确提示使用[官方文档候选](https://docs.bigmodel.cn/cn/guide/start/model-overview)或手填。候选与在线结果分别标注，目录中的模型不代表当前 Key 已开通权限。自定义模型列表地址必须与生成接口同源。目录缓存只存在内存中，重启或更换 Key 后需重新获取。

千问原生调用兼容 `text-generation` 的字符串正文与 `multimodal-generation` 的文本块正文。根据[官方文本生成文档](https://help.aliyun.com/zh/model-studio/text-generation)，已确认的 Qwen3.5/3.6/3.8 通用系列、3.7 Plus/Flash、`qwen3.7-max-2026-06-08` 及 VL 系列会从标准文本端点选择同源多模态端点；保留 `qwen3.7-max`、`qwen3.7-max-2026-05-20`、`qwen3.6-max-preview`、`qwen3.8-2.4t-a95b` 的仅文本例外。显式填写多模态端点可用于新模型；不根据模型名称自动适配 Omni/Realtime、图像生成等其他协议。未知新模型需按其官方文档配置。

统计保存在现有 SQLite 的新增 `llm_usage` 表，每次上游调用尝试记录一次，包括对话、翻译、主备失败和中途失败；运行中的请求结束后才入账。Token 使用厂商返回的累计用量快照，不逐块累加，不以文本长度估算。未返回用量的请求标记为未知（总量仅累加已知部分）。Key 身份使用 Pepper HMAC，不保存明文；更换 Key 保留旧凭证的历史，设备统计按设备 ID 聚合。统计从启用该功能后开始，不回填旧对话，不代表厂商账户在其他服务中的账单；中断流可能只返回部分用量。启动会自动创建新表和索引，不清除设备或对话数据。

ESP32 仍只能修改自己的语音设置。切换大型本地模型时若仍有任务运行，服务会拒绝切换，避免中途释放模型。

配置文件保存在 `VOICE_DATA_DIR/admin_config.enc`，由 `VOICE_CONFIG_KEY` 使用 Fernet 加密；管理员密码使用 PBKDF2-SHA256 摘要。初始密码可在页面中修改。当前 HTTP 只适合短期联调，因为密码和 Key 的传输没有 TLS 保护；申请证书后应将 `VOICE_ADMIN_COOKIE_SECURE=true` 并切换 HTTPS。

管理员页可上传 `esp32_cloud_ota_device/sign_firmware.ps1` 生成的签名 OTA ZIP。生产环境将 `VOICE_FIRMWARE_PUBLIC_BASE_URL` 设置为设备实际访问的 HTTPS 源；它必须与签名清单下载地址完全一致。签名私钥只保留在离线发布电脑，服务器仅使用 `cloud_server/firmware_public_key.pem` 验签。

`/admin/firmware` 提供版本历史、更新说明、最低来源版本和原始签名 ZIP 下载，全部需要管理员登录。新版本按独立目录永久保留，不覆盖旧版；历史版本缺少 ZIP 时，仅在固件和清单重新验签成功后重建下载包。`withdrawn` 版本仍显示，但禁止下载和分发。后台保留的版本不等于全部开发版本：从未归档的固件无法凭版本号恢复。

发布 1.4.2 时必须填写最低来源版本 `1.4.1`，防止向旧 FATFS 分区设备分发新 SPIFFS/模型分区应用。后续同分区版本也必须保留此门槛；请统一从“固件版本”页发布。仓库维护脚本可用 `publish_package(..., activate=False)` 补存已签名的历史包，不移动当前发布指针。每次发布后验证旧来源查询返回 204、兼容来源取得新清单，再核对下载镜像哈希。

语音 ASR 返回空白或纯标点时，任务失败且不进入 LLM、TTS 和历史保存；合法短词如 `I`、`No` 不按文字黑名单拦截。静音与人声的主要判断在设备录音 VAD 完成。

## 电脑端无费用测试

从项目根目录运行自动测试：

```powershell
.\pc_server\.venv\Scripts\python.exe -B -m unittest discover -s .\cloud_server\tests -p "test_*.py" -v
```

启动 mock 服务：

```powershell
.\cloud_server\start_mock_server.ps1
```

第一次先在另一个 PowerShell 注册模拟设备。下面的 Pepper 必须与 `start_mock_server.ps1` 一致：

```powershell
$env:VOICE_AUTH_PEPPER = "local-simulator-only-pepper"
$env:VOICE_DATA_DIR = (Resolve-Path .\cloud_server).Path + "\data"
.\pc_server\.venv\Scripts\python.exe -m cloud_server.cli register-device pc-simulator-01 --name "PC simulator"
```

保存输出的 Token，并运行模拟器：

```powershell
.\pc_server\.venv\Scripts\python.exe -m cloud_server.simulator.esp32_simulator `
  --url http://127.0.0.1:18765 `
  --device-id pc-simulator-01 `
  --token "刚才生成的Token" `
  --question "请用两句话介绍你自己" `
  --output .\cloud_server\output\simulator-answer.wav
```

mock 模式不会调用真实 ASR、TTS 或厂商 API，也不会产生 API 费用。它会完整验证 HTTP、鉴权、任务状态、句级分段、分段长度和 WAV 拼接。

### 本地 VITS 真实压测

可以使用 [sherpa-onnx 官方 VITS 模型页](https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/vits.html)列出的 `sherpa-onnx-vits-zh-ll`（中文、5 个 speaker、16 kHz）验证真实合成。模型放到 `cloud_server/models/sherpa-onnx-vits-zh-ll/` 后执行：

```powershell
.\pc_server\.venv\Scripts\python.exe -m cloud_server.benchmark_tts `
  --model-dir .\cloud_server\models\sherpa-onnx-vits-zh-ll `
  --speaker-ids 0,1,2,3,4 `
  --rounds 3
```

输出包含加载耗时、工作集内存、每轮音频时长和 RTF，并为 5 个音色分别生成 WAV。RTF 小于 1 表示合成速度快于音频播放速度。该社区模型压缩包没有附带明确的商业授权文件，所以只能用于技术压测；产品上线前必须换成许可证清晰且允许目标用途的模型。

2026-08-30 实测结果：本机 5 个中文 speaker 各运行 3 轮，平均 RTF 为 0.354～0.402；双模型混合句生成 4.70 秒音频耗时约 0.76 秒。ECS 同时加载 SenseVoice、中文 VITS 和 904-speaker Piper 英文模型后进程 RSS 约 577 MB，systemd 统计约 733 MB，适合当前单 worker、并发 1 的 2 vCPU/2 GB 配置。

## 生产环境关键配置

复制 `.env.example` 到服务器 `/etc/esp32-voice/voice.env`，至少修改：

```dotenv
VOICE_ALLOW_SIMULATED_INPUT=false
VOICE_AUTH_PEPPER=使用 openssl rand -hex 32 生成
VOICE_DATA_DIR=/var/lib/esp32-voice
VOICE_DATABASE_PATH=/var/lib/esp32-voice/voice.db
VOICE_ASR_PROVIDER=sensevoice
VOICE_TTS_PROVIDER=sherpa_vits
VOICE_LLM_ORDER=deepseek,qwen,glm
DEEPSEEK_API_KEY=...
QWEN_API_KEY=...
GLM_API_KEY=...
SHERPA_ENGLISH_TTS_MODEL=/opt/voice/models/vits-piper-en_US-libritts_r-medium/en_US-libritts_r-medium.onnx
SHERPA_ENGLISH_TTS_TOKENS=/opt/voice/models/vits-piper-en_US-libritts_r-medium/tokens.txt
SHERPA_ENGLISH_TTS_DATA_DIR=/opt/voice/models/vits-piper-en_US-libritts_r-medium/espeak-ng-data
```

模型路径见 `.env.example`。第一版使用单 Uvicorn worker 和 SQLite，是针对当前 2 vCPU/2 GB ECS 的低成本配置；音频合成并发默认为 1，避免 VITS 抢满内存。扩容到多实例时再把 Store 替换为 PostgreSQL/Redis，当前 API 和 ESP32 协议无需改变。

## 阿里云 HTTP 部署顺序

1. 将整个仓库上传/拉取到 `/opt/esp32-voice`，创建 Python 虚拟环境并安装 `cloud_server/requirements.txt`。
2. 创建无登录用户 `voice-gateway`，创建并授权 `/var/lib/esp32-voice` 与 `/etc/esp32-voice/voice.env`。
3. 下载 SenseVoice 与选定的轻量 VITS 模型到 `/opt/voice/models`，在环境文件填写绝对路径。
4. 复制 `deploy/voice-gateway.service` 到 `/etc/systemd/system/`，执行 daemon-reload、enable、start。
5. 先从服务器本机检查 `curl http://127.0.0.1:18765/voice-api/v1/health`。
6. 复制 `deploy/nginx-voice-http.conf` 到 Nginx 站点目录，链接启用后运行 `nginx -t`，成功才 reload。
7. 从公网检查 `http://voice.bsnlch.xyz/voice-api/v1/health`。安全组只开放 80；不要开放 18765。
8. 用 CLI 为每块 ESP32 分别注册设备，把各自 Token 通过加密 BLE 写入 NVS。
9. HTTP 只用于首轮短期联调。Token 会在链路上以明文可见，正式长期使用前必须申请证书并切换 HTTPS；切换时 API 路径不变，只修改 Nginx 与设备 `server_url`。

模型和代码应先下载到本地电脑再通过 SCP 上传 ECS；服务器上的 Python 包安装优先使用阿里云等国内 PyPI 镜像，不让生产服务器直接从 GitHub 下载大模型。

部署与回滚的更详细检查项见 [`../docs/LOW_COST_HTTP_CLOUD_EXECUTION_FLOW.md`](../docs/LOW_COST_HTTP_CLOUD_EXECUTION_FLOW.md)。

## ESP32 代码边界

公网固件在 `esp32_cloud_device/`，不会修改稳定的 `esp32_lan_device/`。它已实现：

- 固定公网域名，不再做 UDP 局域网发现；
- 加密 BLE 配置 Wi-Fi、设备 ID、设备 Token、服务器地址、音量，以及设备独立的中英文音色和语速；
- 首次运行自动迁移局域网固件 `ai-config` NVS 中的 Wi-Fi/音量，并在局域网提供 `http://ESP32-IP/` 配置页；保存时需要串口显示的 6 位设备 PIN，Token 不回显；
- HTTPS 使用 DigiCert Global Root G2 做服务器身份校验，短期站点证书续签无需重刷固件；NTP/HTTPS 不可用时才回退 HTTP；
- 按住 BOOT 录音，提交任务，按序轮询并立即播放每个 PCM 段；
- 网络重试退避、设备归属鉴权、播放时按键中止；
- 串口不打印 Token。

本阶段按要求没有编译、烧录或硬件测试。离线唤醒词需要 ESP-SR/WakeNet 模型和真实板卡内存/误唤醒测试，因此没有伪装成“已验证”功能；当前入口仍是 BOOT 按住说话。

BLE 配置 JSON 可增加以下字段；写入后 ESP32 会用自己的设备凭证同步服务器，失败时可向命令特征写入 `sync_voice` 重试：

```json
{"chinese_speaker_id":2,"english_speaker_id":128,"speech_speed":0.9}
```

## 单条录音诊断

`VOICE_DIAGNOSTIC_DEVICE_ID` 默认为空，不保存输入录音。仅在设备所有者明确同意诊断时设置为该设备 ID：服务器只在私有数据目录的 `diagnostics/latest.wav` 保留该设备最近一次上传的 16 kHz、16 位、单声道录音，下一条直接覆盖；`latest.json` 记录对应任务、接收时间和校验值，不生成按任务或日期归档的输入录音。其他设备及文本请求不会写入此诊断文件。

诊断目录权限为 0700，文件为 0600，没有新增公开下载接口。诊断不改变识别、回答或设备参数，保存失败不会中断正常语音任务。清空该环境变量后停止采集，已有的单条诊断文件需由管理员明确删除。此处限制的是用户输入诊断录音，原有回答语音分段的处理不变。
# API 费用分组与设备查看

计费页面按 API 凭证指纹、模型和用途列出调用量、成功/失败、Token 或语音发送量，以及各币种已记录费用。凭证指纹不包含明文 Key；更换 Key 后旧记录保留，不归到新 Key。语言模型与云端语音费用分别合计，未回传用量或未计价调用明确标记，不能当作免费调用。旧语音记录缺少凭证信息，单独标注为未归属。

设备可使用现有设备凭证读取 `GET /voice-api/v1/device-usage?period=30d`。周期支持 `all`、`24h`、`7d`、`30d`，接口只汇总已鉴权设备的记录，不接受切换设备参数。最多返回 40 个分组，费用合计仍包含所选周期内全部记录，多余历史分组在服务器查看。响应禁止缓存，不包含其他设备明细、接口地址或明文 Key。设备网页“语音与音量”中的“本设备 API 用量与费用”通过设备代理读取该接口，升级固件后生效。

`view=compact` 返回设备网页需要的精简字段，费用仅返回人民币字符串，使用 Decimal 四舍五入保留两位小数；合计先按原始金额累加再舍入，不累加已舍入明细。非人民币费用不换算，未计价、未回传用量和不完整费用仍有提示。默认 `view=full` 与服务器账本保留原始精度及币种。ESP32 流式代理，无用量文件或 NVS 记录，也不增加实体屏幕内容。
该统计只覆盖本服务记录的调用，不是厂商余额查询；未抵扣赠送额度、折扣和资源包，实际扣费以厂商账单为准。费用按调用时规则快照记录，调整规则不改历史金额。

## 设备个人记忆备份（2026-10-02）

认证后的 `GET/PUT /voice-api/v1/personal-profile` 读写本设备独立的特征快照。字段包括 `schema: 1`、无符号 32 位 `revision`、三个布尔开关 `enabled/auto_collect/backup_enabled`，以及最多 24 个 `items`（正整数 `id`、`category`、180 UTF-8 字节内 `value`、`explicit/observed` 来源）。类别限 `name/interest/communication/goal/habit`；禁止额外字段和类型隐式转换。

同版本同内容重试允许，旧版本或同版本不同内容返回 409；写入在 SQLite 事务内执行。关备份时清除持久条目，但保留版本、策略和不可逆内容指纹以拒绝旧快照覆盖。设备 `/local-result` 可附带同一快照，成功响应增加 `profile_revision`；关备份但开记忆的快照仅为本次回答提供上下文。

使用个人快照的任务不再拼接旧对话记忆，避免已经忘记的特征再次出现。特征作为不可信数据提供给现有模型，本轮用户问题优先；没有训练或升级模型。原有任务历史和诊断录音留存策略不变。旧固件不附快照仍兼容。

先更新云端再安装新设备固件，表在启动时自动创建。设备界面、隐私开关和使用说明见 [单麦克风陪伴功能](../esp32_cloud_ota_device/COMPANION_FEATURES.md)。本次修改未部署。
