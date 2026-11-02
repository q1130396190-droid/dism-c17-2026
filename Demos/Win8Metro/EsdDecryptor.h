// EsdDecryptor.h
// [知识库 41 §26] 加密 ESD 自主解密（不依赖 dism.exe 壳）
//
// 复刻链路（原版 Dism++ FUN_14002648c，RVA 0x2648c）：
//   读 208 字节 WIM 头 → 定位 xml_data 资源 → 解析
//   <ENCRYPTED><KEY>base64…</KEY><RANGE Offset Bytes/>…</ENCRYPTED>
//   base64 解码 KEY 并逆序 → 构造 CryptoAPI SIMPLEBLOB（12 字节头）
//   CryptAcquireContextW(PROV_RSA_AES, CRYPT_VERIFYCONTEXT)
//   Data.xml <ESDDecryptKey> 私钥逐条 CryptImportKey(CRYPT_OAEP) 尝试
//   逐 RANGE：CryptImportKey 解包 AES 会话密钥 → CryptDecrypt AES-CBC
//   就地解密 → CryptDestroyKey
//   尾写 WIM 头 + XML 资源（与原版收尾一致）
#pragma once
#include <string>
#include <vector>

namespace EsdDecryptor {

// 探测文件是否为含 ENCRYPTED/RANGE 节点的加密 ESD。
// 仅读头部与 XML 资源，不产生输出文件。
[[nodiscard]] bool IsEncryptedEsd(const std::wstring& path);

// 将加密 ESD 解密为同尺寸、已去加密的 WIM（仍为 LZMS 固态压缩，
// 可直接被 wimgapi/WIMCreateFile 打开）。
//   base64Keys : Data.xml <ESDDecryptKey> 的若干 base64 私钥，逐条尝试。
//   pErr       : 失败时回填中文错误说明（可为 nullptr）。
[[nodiscard]] bool DecryptToFile(const std::wstring& srcEsd,
                   const std::wstring& dstWim,
                   const std::vector<std::wstring>& base64Keys,
                   std::wstring* pErr);

} // namespace EsdDecryptor
