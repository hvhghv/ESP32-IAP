/*
 * ESP IAP - lwIP 自定义钩子
 *
 * 本文件通过 ESP_IDF_LWIP_HOOK_FILENAME 机制被 lwIP 组件自动包含
 * (见 components/lwip/port/include/lwip_default_hooks.h)。
 *
 * 在 main/CMakeLists.txt 中通过以下方式启用:
 *     idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
 *     target_compile_options(${lwip_lib} PRIVATE "-I${CMAKE_CURRENT_LIST_DIR}")
 *     target_compile_definitions(${lwip_lib} PRIVATE
 *         "-DESP_IDF_LWIP_HOOK_FILENAME=\"iap_lwip_hooks.h\"")
 *
 * ===========================================================================
 * 目的: 让 DHCP 服务器"不下发网关与 DNS"
 * ===========================================================================
 *
 * 背景:
 *   ESP-IDF 的 esp_netif_dhcps_option() 在本场景下无法可靠禁用网关:
 *     1) 启动前调用: dhcps_start() 会把 opt_info 重置为默认值(含 OFFER_ROUTER);
 *     2) 启动后调用: 返回 ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED。
 *
 * ---------------------------------------------------------------------------
 * 为什么需要两个钩子
 * ---------------------------------------------------------------------------
 *   dhcpserver.c 中 handle_dhcp() 的流程:
 *
 *       parse_msg()                        // 解析请求
 *       LWIP_HOOK_DHCPS_POST_STATE()       // (A) 此时 options 仍是"请求"内容
 *       switch (state) {
 *         case OFFER: send_offer();        // 内部才生成响应选项
 *         case ACK:   send_ack();
 *       }
 *
 *   send_offer()/send_ack() 内部:
 *
 *       create_msg()                       // 重置 options, 只写 magic cookie
 *       add_msg_type(&m->options[4], ...)  // 写 option 53
 *       add_offer_options()                // 写 1/51/54/3(ROUTER)/6(DNS)/28/26
 *       LWIP_HOOK_DHCPS_POST_APPEND_OPTS() // (B) 选项已写完, 可安全裁剪
 *       add_end()                          // 写 option 255
 *       udp_sendto()                       // 发送
 *
 *   结论:
 *     - (A) POST_STATE 时响应选项尚未生成, 在此删除无效;
 *     - (B) POST_APPEND_OPTS 才是正确位置, 但它只提供"选项区末尾指针",
 *       不提供起始地址。
 *
 *   因此采用组合方案:
 *     - (A) POST_STATE 拿到 struct dhcps_msg *msg, 记录 options 基址;
 *     - (B) POST_APPEND_OPTS 用记录的基址 + 末尾指针, 精确裁剪选项区。
 *
 *   两个钩子在同一次 handle_dhcp() 调用链中、同一 TCP/IP 线程内执行,
 *   因此用静态变量传递基址是安全的(无需加锁)。
 *
 * ---------------------------------------------------------------------------
 * 为什么必须删除网关
 * ---------------------------------------------------------------------------
 *   若 DHCP 下发网关, 客户端会安装默认路由 0.0.0.0/0 -> AP_IP,
 *   把本应发往其他网络(如互联网)的流量全部送到本设备,
 *   导致客户端网络中断。本设备只提供本地 HTTP 服务, 无需充当网关。
 *
 * 删除后客户端仍会获得:
 *   - IP 地址    (yiaddr / option 50)
 *   - 子网掩码   (option 1)
 *   - 租期       (option 51)
 *   - 服务器标识 (option 54)
 *   - 广播地址   (option 28)
 *   - MTU        (option 26)
 * 因此可正常访问本设备(同一子网内), 但不会劫持默认路由。
 */

#ifndef IAP_LWIP_HOOKS_H
#define IAP_LWIP_HOOKS_H

#include <string.h>
#include <stdint.h>
#include "esp_log.h"

/* DHCP 选项编号 */
#define IAP_DHCP_OPT_PAD        0       /*!< 填充字节 */
#define IAP_DHCP_OPT_SUBNET     1       /*!< 子网掩码 */
#define IAP_DHCP_OPT_ROUTER     3       /*!< 路由器 (默认网关) */
#define IAP_DHCP_OPT_DNS        6       /*!< 域名服务器 */
#define IAP_DHCP_OPT_MTU        26      /*!< 接口 MTU */
#define IAP_DHCP_OPT_BROADCAST  28      /*!< 广播地址 */
#define IAP_DHCP_OPT_MSGTYPE    53      /*!< 消息类型 */
#define IAP_DHCP_OPT_SERVERID   54      /*!< 服务器标识 */
#define IAP_DHCP_OPT_END        255     /*!< 选项结束标记 */

/* DHCP 魔术 cookie 长度 (0x63 0x82 0x53 0x63) */
#define IAP_DHCP_COOKIE_LEN     4

/* dhcps_msg.options 在结构体中的偏移 (op/htype/hlen/hops=4, xid=4,
 * secs+flags=4, ciaddr/yiaddr/siaddr/giaddr=16, chaddr=16, sname=64,
 * file=128 -> 合计 240) */
#define IAP_DHCPS_MSG_OPTIONS_OFFSET 240

/* 诊断日志开关: 置 1 打印 DHCP 报文选项详情 (调试用) */
#define IAP_DHCP_DIAG_LOG       1

/*
 * 完整报文 hexdump 开关 (调试用)
 *
 * 打印整个 dhcps_msg (552 字节) 的十六进制, 用于排查:
 *   - yiaddr / siaddr / giaddr 等头部字段是否正确
 *   - 选项区之外是否有残留数据
 *
 * 注意: 输出量大 (每条报文 ~35 行), 仅在排查时临时开启。
 */
#define IAP_DHCP_FULL_HEXDUMP   1

static const char *IAP_DHCP_TAG = "iap_dhcp";

/* 选项名称 (仅用于日志可读性) */
static inline const char *iap_dhcp_opt_name(uint8_t id)
{
    switch (id) {
    case IAP_DHCP_OPT_SUBNET:    return "SubnetMask";
    case IAP_DHCP_OPT_ROUTER:    return "ROUTER(gw)";
    case IAP_DHCP_OPT_DNS:       return "DNS";
    case IAP_DHCP_OPT_MTU:       return "MTU";
    case IAP_DHCP_OPT_BROADCAST: return "Broadcast";
    case IAP_DHCP_OPT_MSGTYPE:   return "MsgType";
    case IAP_DHCP_OPT_SERVERID:  return "ServerID";
    default:                     return "?";
    }
}

/**
 * @brief 打印选项区内容 (TLV 解析 + 十六进制)
 *
 * @param stage 阶段标签 ("BEFORE" / "AFTER")
 * @param opts  选项区起始 (跳过 magic cookie)
 * @param end   选项区结束
 */
static inline void iap_dhcp_dump_options(const char *stage,
                                         const uint8_t *opts,
                                         const uint8_t *end)
{
#if IAP_DHCP_DIAG_LOG
    ESP_LOGI(IAP_DHCP_TAG, "---- %s: 选项区 (%d 字节) ----",
             stage, (int)(end - opts));

    const uint8_t *p = opts;
    while (p < end) {
        uint8_t type = p[0];

        if (type == IAP_DHCP_OPT_END) {
            ESP_LOGI(IAP_DHCP_TAG, "  [%3u] END", type);
            break;
        }
        if (type == IAP_DHCP_OPT_PAD) {
            p++;
            continue;
        }
        if (p + 1 >= end) {
            ESP_LOGW(IAP_DHCP_TAG, "  [%3u] 截断 (缺长度)", type);
            break;
        }

        uint8_t len = p[1];
        const uint8_t *next = p + 2 + (size_t)len;
        if (next > end) {
            ESP_LOGW(IAP_DHCP_TAG, "  [%3u] 长度越界 (len=%u)", type, len);
            break;
        }

        /* 对已知的 IP 类选项打印点分十进制 */
        if ((type == IAP_DHCP_OPT_SUBNET || type == IAP_DHCP_OPT_ROUTER ||
             type == IAP_DHCP_OPT_DNS || type == IAP_DHCP_OPT_SERVERID ||
             type == IAP_DHCP_OPT_BROADCAST) && len == 4) {
            ESP_LOGI(IAP_DHCP_TAG, "  [%3u] %-12s = %u.%u.%u.%u",
                     type, iap_dhcp_opt_name(type),
                     p[2], p[3], p[4], p[5]);
        } else if (type == IAP_DHCP_OPT_MSGTYPE && len == 1) {
            const char *mt = "?";
            switch (p[2]) {
            case 1: mt = "DISCOVER"; break;
            case 2: mt = "OFFER";    break;
            case 3: mt = "REQUEST";  break;
            case 5: mt = "ACK";      break;
            case 6: mt = "NAK";      break;
            default: break;
            }
            ESP_LOGI(IAP_DHCP_TAG, "  [%3u] %-12s = %u (%s)",
                     type, iap_dhcp_opt_name(type), p[2], mt);
        } else {
            /* 其它选项: 打印十六进制 */
            char hex[3 * 32 + 1];
            int n = 0;
            for (uint8_t i = 0; i < len && i < 32; i++) {
                n += snprintf(hex + n, sizeof(hex) - n, "%02x ", p[2 + i]);
            }
            hex[n] = '\0';
            ESP_LOGI(IAP_DHCP_TAG, "  [%3u] %-12s = %s(len=%u)",
                     type, iap_dhcp_opt_name(type), hex, len);
        }

        p = next;
    }
    ESP_LOGI(IAP_DHCP_TAG, "---- %s 结束 ----", stage);
#else
    (void)stage; (void)opts; (void)end;
#endif
}

/**
 * @brief 打印完整 DHCP 报文 (hexdump + 关键头部字段)
 *
 * 报文布局 (dhcps_msg, 552 字节):
 *   0..3    op/htype/hlen/hops
 *   4..7    xid
 *   8..11   secs/flags
 *   12..27  ciaddr/yiaddr/siaddr/giaddr
 *   28..43  chaddr (16)
 *   44..107 sname  (64)
 *   108..235 file  (128)
 *   236..239 magic cookie
 *   240..551 options (312)
 *
 * @param stage 阶段标签
 * @param base  报文起始地址 (msg->options - 240)
 */
static inline void iap_dhcp_dump_packet(const char *stage, const uint8_t *base)
{
#if IAP_DHCP_FULL_HEXDUMP
    if (base == NULL) {
        return;
    }
    const uint8_t *m = base - IAP_DHCPS_MSG_OPTIONS_OFFSET;

    ESP_LOGI(IAP_DHCP_TAG, "===== 完整报文 %s (552 字节) =====", stage);

    /* 关键头部字段 */
    ESP_LOGI(IAP_DHCP_TAG, "  op=%u htype=%u hlen=%u hops=%u",
             m[0], m[1], m[2], m[3]);
    ESP_LOGI(IAP_DHCP_TAG, "  xid=%02x%02x%02x%02x",
             m[4], m[5], m[6], m[7]);
    ESP_LOGI(IAP_DHCP_TAG, "  secs=%02x%02x flags=%02x%02x",
             m[8], m[9], m[10], m[11]);
    ESP_LOGI(IAP_DHCP_TAG, "  ciaddr = %u.%u.%u.%u",
             m[12], m[13], m[14], m[15]);
    ESP_LOGI(IAP_DHCP_TAG, "  yiaddr = %u.%u.%u.%u   <-- 分配给客户端的 IP",
             m[16], m[17], m[18], m[19]);
    ESP_LOGI(IAP_DHCP_TAG, "  siaddr = %u.%u.%u.%u",
             m[20], m[21], m[22], m[23]);
    ESP_LOGI(IAP_DHCP_TAG, "  giaddr = %u.%u.%u.%u",
             m[24], m[25], m[26], m[27]);
    ESP_LOGI(IAP_DHCP_TAG, "  chaddr = %02x:%02x:%02x:%02x:%02x:%02x",
             m[28], m[29], m[30], m[31], m[32], m[33]);
    ESP_LOGI(IAP_DHCP_TAG, "  cookie = %02x %02x %02x %02x",
             m[236], m[237], m[238], m[239]);

    /* 全报文 hexdump (每行 16 字节) */
    for (int off = 0; off < 552; off += 16) {
        char line[3 * 16 + 1];
        int n = 0;
        for (int i = 0; i < 16; i++) {
            n += snprintf(line + n, sizeof(line) - n, "%02x ", m[off + i]);
        }
        line[n] = '\0';
        ESP_LOGI(IAP_DHCP_TAG, "  %03d: %s", off, line);
    }
    ESP_LOGI(IAP_DHCP_TAG, "===== 报文结束 =====");
#else
    (void)stage; (void)base;
#endif
}

/*
 * 选项区基址暂存 (由 POST_STATE 写入, 由 POST_APPEND_OPTS 读取)。
 *
 * 说明:
 *   - 两个钩子在同一次 handle_dhcp() 调用中顺序执行, 同一线程;
 *   - 使用 volatile 防止编译器跨钩子优化掉该变量;
 *   - 初始为 NULL, POST_APPEND_OPTS 中会做非空检查。
 */
static uint8_t *volatile iap_dhcp_opts_base = NULL;

/**
 * @brief 从选项区中删除指定选项
 *
 * 选项区为 TLV 序列: <type(1)> <len(1)> <value(len)> ... 
 * 以 255 (END) 结束, 0 (PAD) 为单字节填充。
 *
 * 删除方式: 将后续内容整体前移 total 字节, 并在尾部补 PAD。
 *
 * 重要: 通过 end_io 直接更新调用方的"选项区末尾"指针。
 *   不能用 iap_dhcp_options_len() 重新扫描来求新末尾 ——
 *   因为尾部补的 PAD(0x00) 会被扫描函数当作可跳过字节,
 *   导致一直走到物理末尾, 返回错误的(过长的)长度,
 *   最终 add_end() 把 END 写在错误位置, 报文尾部残留垃圾,
 *   客户端会拒绝 OFFER (表现为反复 DISCOVER 不发 REQUEST)。
 *
 * @param opts   选项区起始地址 (跳过 magic cookie 之后的第一个选项)
 * @param end_io 输入: 选项区结束地址; 输出: 删除后的新结束地址
 * @param opt_id 要删除的选项编号
 * @return 实际删除的选项个数
 */
static inline int iap_dhcp_strip_option(uint8_t *opts, uint8_t **end_io,
                                        uint8_t opt_id)
{
    uint8_t *p = opts;
    uint8_t *end = *end_io;
    int removed = 0;

    while (p < end) {
        uint8_t type = p[0];

        /* END: 停止扫描 */
        if (type == IAP_DHCP_OPT_END) {
            break;
        }
        /* PAD: 单字节, 跳过 */
        if (type == IAP_DHCP_OPT_PAD) {
            p++;
            continue;
        }

        /* 越界保护: 至少需要 type + len 两个字节 */
        if (p + 1 >= end) {
            break;
        }

        uint8_t len = p[1];
        uint8_t *next = p + 2 + (size_t)len;    /* 下一选项位置 */

        /* 越界保护 */
        if (next > end) {
            break;
        }

        if (type == opt_id) {
            /* 命中: 将 next..end 内容前移到 p */
            size_t remain = (size_t)(end - next);
            if (remain > 0) {
                memmove(p, next, remain);
            }
            /* 尾部补 PAD, 并缩短结束位置 */
            size_t total = (size_t)(next - p);
            memset(end - total, IAP_DHCP_OPT_PAD, total);
            end -= total;
            removed++;
            /* p 不移动, 继续检查前移过来的内容 */
            continue;
        }

        p = next;
    }

    *end_io = end;
    return removed;
}

/**
 * @brief 把 END 标记之后的所有字节清零
 *
 * 为什么必须清零:
 *   send_offer()/send_ack() 中:
 *       p = dhcps_pbuf_alloc(len);       // len = malloc_len (整个报文长度)
 *       while (q) { for (i=0;i<q->len;i++) data[i] = ((u8_t*)m)[cnt++]; }
 *
 *   发送的是"整个 dhcps_msg 结构体"(552 字节), 而不是"选项区实际长度"。
 *   删除 Router/DNS 后 add_end() 在更早位置写 END, 但后续字节仍是
 *   add_offer_options() 写入的残留数据(含被删除选项的字节)。
 *   客户端可能因报文尾部存在垃圾数据而拒绝 OFFER。
 *
 *   因此必须把 END 之后的字节全部清零。
 *
 * @param end    END 标记位置 (add_end 写入后)
 * @param limit  选项区物理末尾 (options 数组结束处)
 */
static inline void iap_dhcp_zero_tail(uint8_t *end, const uint8_t *limit)
{
    if (end < limit) {
        memset(end, 0, (size_t)(limit - end));
    }
}

/**
 * @brief 钩子 (A): 记录选项区基址
 *
 * 在 handle_dhcp() 中 parse_msg() 之后调用。
 * 此时 msg 是收到的请求报文, 但其 options 字段地址与后续
 * send_offer()/send_ack() 使用的 m->options 完全相同
 * (二者是同一个 pmsg_dhcps), 因此可安全记录基址。
 *
 * @param msg   收到的 DHCP 报文
 * @param len   报文长度
 * @param state 解析状态
 * @return 原样返回 state
 */
#define LWIP_HOOK_DHCPS_POST_STATE(msg, len, state)                     \
    ({                                                                  \
        s16_t _iap_state = (state);                                     \
        /* 记录 options 基址, 供 POST_APPEND_OPTS 使用 */               \
        iap_dhcp_opts_base = (uint8_t *)((msg)->options);               \
        /* 诊断: 打印收到的请求选项 (此时 options 是请求内容) */        \
        if (IAP_DHCP_DIAG_LOG) {                                        \
            ESP_LOGI(IAP_DHCP_TAG,                                      \
                     "=== 收到 DHCP 请求: state=%d, len=%u ===",        \
                     (int)_iap_state, (unsigned)(len));                 \
            iap_dhcp_dump_options("请求选项",                           \
                                  (uint8_t *)((msg)->options) +         \
                                      IAP_DHCP_COOKIE_LEN,              \
                                  (uint8_t *)((msg)->options) +         \
                                      sizeof((msg)->options));          \
        }                                                               \
        _iap_state;                                                     \
    })

/**
 * @brief 钩子 (B): 裁剪选项区, 删除 Router 与 DNS
 *
 * 在 send_offer()/send_ack() 中 add_offer_options() 之后、
 * add_end() 之前调用。
 *
 * 参数:
 *   netif   - 网络接口
 *   dhcps   - DHCP 服务器实例
 *   state   - DHCP 消息类型 (DHCPOFFER=2 / DHCPACK=5 / DHCPNAK=6)
 *   pp_opts - 指向"选项区末尾指针"的指针, *pp_opts 为下一个可写位置
 */
/*
 * 注意: dhcpserver.c 中的调用点没有分号:
 *     LWIP_HOOK_DHCPS_POST_APPEND_OPTS(...)
 *     end = add_end(end);
 * 因此宏必须展开为一个完整语句块(裸块), 不能是表达式或 do-while。
 * 这里用 { ... } 形式, 后面紧跟的语句仍然合法。
 */
#define LWIP_HOOK_DHCPS_POST_APPEND_OPTS(netif, dhcps, state, pp_opts)  \
    {                                                                   \
        uint8_t *_iap_base = iap_dhcp_opts_base;                        \
        uint8_t *_iap_end  = (uint8_t *)(*(pp_opts));                   \
        /* 跳过 4 字节 magic cookie, 得到第一个选项的地址 */            \
        uint8_t *_iap_opts = (_iap_base != NULL)                        \
                             ? (_iap_base + IAP_DHCP_COOKIE_LEN) : NULL;\
        /* 选项区物理末尾 (options 数组结束处) */                       \
        uint8_t *_iap_limit = (_iap_base != NULL)                       \
                              ? (_iap_base + 312) : NULL;               \
        /*                                                              \
         * 仅处理 OFFER 与 ACK: 这两个消息才会携带地址配置选项。        \
         * NAK 不含这些选项, 无需处理。                                 \
         */                                                             \
        if (_iap_opts != NULL && _iap_opts < _iap_end &&                \
            ((state) == 2 /* DHCPOFFER */ || (state) == 5 /* DHCPACK */)) { \
            /* 诊断: 打印裁剪前的完整选项 (这是即将发送的内容) */       \
            if (IAP_DHCP_DIAG_LOG) {                                    \
                ESP_LOGI(IAP_DHCP_TAG,                                  \
                         "=== 发送 DHCP %s (裁剪前) ===",               \
                         (state) == 2 ? "OFFER" : "ACK");               \
                iap_dhcp_dump_options("裁剪前", _iap_opts, _iap_end);   \
            }                                                           \
                                                                        \
            int _iap_n = 0;                                             \
            uint8_t *_iap_cur_end = _iap_end;                           \
            _iap_n += iap_dhcp_strip_option(_iap_opts, &_iap_cur_end,   \
                                            IAP_DHCP_OPT_ROUTER);       \
            _iap_n += iap_dhcp_strip_option(_iap_opts, &_iap_cur_end,   \
                                            IAP_DHCP_OPT_DNS);          \
            /*                                                          \
             * 裁剪后选项区变短, 用 strip_option 回传的新末尾。          \
             * 注意: 后续 add_end() 会在 *pp_opts 处写 END,             \
             * 因此这里必须把"新末尾"之后的字节全部清零,                \
             * 否则发送整个 dhcps_msg 时会带上残留的旧选项数据。        \
             */                                                         \
            if (_iap_n > 0) {                                           \
                /* 清零 END 之后 (add_end 将写入的位置之后) 的全部字节 */\
                iap_dhcp_zero_tail(_iap_cur_end, _iap_limit);           \
                *(pp_opts) = _iap_cur_end;                              \
            }                                                           \
                                                                        \
            /* 诊断: 打印裁剪后的选项 + 结果统计 */                     \
            if (IAP_DHCP_DIAG_LOG) {                                    \
                ESP_LOGI(IAP_DHCP_TAG,                                  \
                         "=== 发送 DHCP %s (裁剪后, 删除 %d 个选项) ===",\
                         (state) == 2 ? "OFFER" : "ACK", _iap_n);       \
                iap_dhcp_dump_options("裁剪后", _iap_opts, *(pp_opts)); \
                ESP_LOGI(IAP_DHCP_TAG,                                  \
                         "  END 之后已清零 %d 字节",                    \
                         (int)(_iap_limit - *(pp_opts)));               \
            }                                                           \
            /*                                                          \
             * 诊断: 完整报文 hexdump (含 op/yiaddr/chaddr 等头部字段)  \
             * 用于排查客户端拒绝 OFFER 的原因。                        \
             */                                                         \
            if (IAP_DHCP_FULL_HEXDUMP) {                                \
                iap_dhcp_dump_packet((state) == 2 ? "OFFER" : "ACK",    \
                                     _iap_base);                        \
            }                                                           \
        } else if (IAP_DHCP_DIAG_LOG) {                                 \
            ESP_LOGW(IAP_DHCP_TAG,                                      \
                     "POST_APPEND_OPTS: 跳过 (base=%p, state=%d)",      \
                     (void *)_iap_base, (int)(state));                  \
        }                                                               \
        (void)(netif);                                                  \
        (void)(dhcps);                                                  \
    }

#endif /* IAP_LWIP_HOOKS_H */
