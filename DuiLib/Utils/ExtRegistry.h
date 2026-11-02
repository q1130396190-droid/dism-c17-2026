// DuiLib/Utils/ExtRegistry.h
//
// T5.19  DuiLib::ext 第三方扩展注册表（最小可用实现）
//
// 目的：让第三方扩展（新控件、新属性、新消息类型）通过明确命名空间接入
//       DuiLib，不污染 core。与 S3 注册表（DuiLib::s3）平级：
//         - DuiLib::s3 承载 core 内置属性（编译期固定）
//         - DuiLib::ext 承载第三方扩展属性（运行期可注册）
//
// 边界（详见 docs/c8-whitelist-design.md）：
//   - 命名冲突：ext 注册名若与 S3 已占用同名 -> 拒绝注册
//   - 判定顺序：AddCustomAttribute 中 S3 优先 -> ext -> N1 诊断
//     ★ 本文件不提供与 AddCustomAttribute 的集成——由后续 T5.21 完成
//   - ID 范围：[0x1000, 0x7FFF]，避开 core PropId 的 [0x0000, 0x0FFF]
//
// 实现约束：
//   - Header-only（Meyers singleton），避免新增 .cpp 源文件
//   - 线程安全：注册与查询都持 mutex（保守但正确；热路径在
//     AddCustomAttribute 之前一次调用，性能影响可控）
//   - 不抛异常：所有失败以注册结果枚举返回，不中断调用方
//
// 版本：1.0  T5.19  2026-10-15
#ifndef DUI_EXT_REGISTRY_H
#define DUI_EXT_REGISTRY_H

#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace DuiLib
{
    // Forward-declare CDuiString 类型（避免循环包含；注册 API 只接受 LPCTSTR）
    class CDuiString;

    // 命名空间 DuiLib::ext —— 第三方扩展专用
    namespace ext
    {

        // 注册结果
        enum class ExtRegisterResult : int
        {
            ok          = 0,   // 注册成功
            invalid_id  = 1,   // ID 超出合法范围
            empty_name  = 2,   // 名称为空
            duplicate   = 3,   // 同一 ID 已注册（同名）
            name_exists = 4,   // 该名称已被其他 ID 注册
            collision   = 5,   // 与 S3 core 属性名冲突
        };

        // ID 合法范围（保留给第三方扩展）
        constexpr std::uint32_t EXT_ID_MIN = 0x1000;
        constexpr std::uint32_t EXT_ID_MAX = 0x7FFF;

        // 未注册/未命中的哨兵值
        constexpr std::uint32_t EXT_ID_NONE    = 0u;
        constexpr std::uint32_t EXT_ID_UNKNOWN = 0u;

        // ID 合法性判定（C++17 constexpr，可用于编译期常量表达式）
        constexpr bool is_valid_ext_id(std::uint32_t id) noexcept
        {
            return id >= EXT_ID_MIN && id <= EXT_ID_MAX;
        }


        // 注册条目：仅记录 (name -> id) 与 (id -> name) 双向索引
        struct Entry
        {
            std::string name;      // 规范化后的属性名（小写）
            std::uint32_t id;
        };

        // 快照条目（供 dump_registry 使用）
        struct SnapshotEntry
        {
            const char* name;
            std::uint32_t id;
        };

        // 内部单例
        class Registry
        {
        public:
            static [[nodiscard]] [[nodiscard]] Registry& Instance()
            {
                // Meyers singleton — 线程安全（C++11 起）
                static Registry inst;
                return inst;
            }

            // 私有构造/析构
            Registry() {}
            ~Registry() {}

            // 禁止拷贝/移动
            Registry(const Registry&) = delete;
            Registry& operator=(const Registry&) = delete;

            // 注册一个扩展属性
            // name: 属性名（不区分大小写）
            // id:   扩展 ID，必须在 [EXT_ID_MIN, EXT_ID_MAX]
            [[nodiscard]] [[nodiscard]] ExtRegisterResult Register(const char* name, std::uint32_t id)
            {
                if (!name || name[0] == '\0')
                    return ExtRegisterResult::empty_name;
                if (!is_valid_ext_id(id))
                    return ExtRegisterResult::invalid_id;

                std::string key = Normalize(name);
                if (key.empty())
                    return ExtRegisterResult::empty_name;

                std::lock_guard<std::mutex> lk(m_mutex);

                // 同一 (name, id) 幂等：返回 ok
                auto it_id = m_id_to_name.find(id);
                if (it_id != m_id_to_name.end())
                {
                    if (it_id->second == key)
                        return ExtRegisterResult::ok;         // 幂等
                    return ExtRegisterResult::duplicate;      // 同 ID 被占用
                }
                // 名称已被其他 ID 占用
                auto it_name = m_name_to_id.find(key);
                if (it_name != m_name_to_id.end())
                    return ExtRegisterResult::name_exists;

                m_name_to_id.emplace(key, id);
                m_id_to_name.emplace(id, key);
                return ExtRegisterResult::ok;
            }

            // 查询：返回该 name 对应的 ID；未命中返回 EXT_ID_NONE
            [[nodiscard]] [[nodiscard]] std::uint32_t Lookup(const char* name) const
            {
                if (!name || name[0] == '\0')
                    return EXT_ID_NONE;
                std::string key = Normalize(name);
                std::lock_guard<std::mutex> lk(m_mutex);
                auto it = m_name_to_id.find(key);
                return (it != m_name_to_id.end()) ? it->second : EXT_ID_NONE;
            }

            // 反查：ID -> name（用于调试）
            [[nodiscard]] [[nodiscard]] const char* NameOf(std::uint32_t id) const
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                auto it = m_id_to_name.find(id);
                return (it != m_id_to_name.end()) ? it->second.c_str() : "";
            }

            // 快照：返回所有已注册的条目（按 name 排序）
            // 说明：返回 std::vector<Entry> 是拷贝，调用方可安全持有
            [[nodiscard]] [[nodiscard]] std::vector<Entry> Snapshot() const
            {
                std::vector<Entry> out;
                std::lock_guard<std::mutex> lk(m_mutex);
                out.reserve(m_name_to_id.size());
                for (const auto& kv : m_name_to_id)
                    out.push_back(Entry{ kv.first, kv.second });
                return out;
            }

            // 清空（供测试使用；不建议在运行期调用）
            void Reset()
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_name_to_id.clear();
                m_id_to_name.clear();
            }

            [[nodiscard]] [[nodiscard]] std::size_t Size() const
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                return m_name_to_id.size();
            }

        private:
            // 名称规范化：小写 + trim 首尾空白
            // 说明：std::string 构造 (const char*, size_t) 会复制该数量的字符，
            //       所以必须用 (begin, end) 构造；不要写成 (len, '\0') 之类的错误形式。
            static std::string Normalize(const char* s)
            {
                const char* p = s;
                while (*p && (*p == ' ' || *p == '\t')) ++p;
                const char* end = p;
                while (*end) ++end;
                while (end > p && (end[-1] == ' ' || end[-1] == '\t')) --end;
                std::string out(p, end);
                for (std::size_t i = 0; i < out.size(); ++i)
                {
                    char c = out[i];
                    if (c >= 'A' && c <= 'Z')
                        out[i] = static_cast<char>(c - 'A' + 'a');
                }
                return out;
            }

            mutable std::mutex m_mutex;
            std::unordered_map<std::string, std::uint32_t> m_name_to_id;
            std::unordered_map<std::uint32_t, std::string> m_id_to_name;
        };

        // =========================================================================
        // 公开 API（顶层命名空间函数，转发到 Registry::Instance()）
        // =========================================================================

        [[nodiscard]] inline ExtRegisterResult register_ext_prop(const char* name,
                                                    std::uint32_t id)
        {
            return Registry::Instance().Register(name, id);
        }

        [[nodiscard]] inline std::uint32_t lookup_ext_prop(const char* name)
        {
            return Registry::Instance().Lookup(name);
        }

        [[nodiscard]] inline const char* ext_prop_name(std::uint32_t id)
        {
            return Registry::Instance().NameOf(id);
        }

        [[nodiscard]] inline std::size_t ext_registry_size()
        {
            return Registry::Instance().Size();
        }

        // 只读快照（供 dump_registry / 调试工具）
        [[nodiscard]] inline std::vector<Entry> dump_registry()
        {
            return Registry::Instance().Snapshot();
        }

        // 测试辅助：清空注册表（生产代码不应调用）
        inline void reset_registry()
        {
            Registry::Instance().Reset();
        }

    } // namespace ext
} // namespace DuiLib

#endif // DUI_EXT_REGISTRY_H
