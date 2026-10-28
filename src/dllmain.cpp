// EvilNunModMenu DLL 入口
#include "common.h"
#include "renderer.h"
#include "menu.h"
#include "game_context.h"

#include <SDK.hpp>
#include <vector>
#include <string>
#include <psapi.h>
#include "MinHook.h"

static HMODULE g_hModule = nullptr;
static DWORD g_MainThreadId = 0;
static uintptr_t g_GameBase = 0, g_GameSize = 0;

// ============ 结算崩溃止血 hook (EXE+0x17B9CB0) ============
// 反汇编确认的崩溃链: 结算时游戏校验器(0x3C3E5A0)遍历所有 UWorld 发现"孤儿 World"
// → 错误报告函数(0x3C25C60)收集两个关联对象后, 对第二个 ([rbp-0x50]) 不做 null 检查
// 直接调用本函数取全名 → this=null 时读 [this+8] → AV 读 0x8。
// 调用者对输出有 null-safe 分支 (FString 长度 0 → 显示 "None"), 因此 this==null 时
// 清零输出 FString 并原样返回即可安全短路, 游戏继续运行。
using GetFullNameLike_t = void* (*)(void* This, void* OutFString);
static GetFullNameLike_t o_GetFullNameLike = nullptr;
static int s_NullFullNameHits = 0;

static void* hk_GetFullNameLike(void* This, void* OutFString)
{
    if (!This)
    {
        if (OutFString)
            memset(OutFString, 0, 16); // FString {Data=0, Num=0, Max=0}
        if (s_NullFullNameHits < 5)
        {
            s_NullFullNameHits++;
            ModMenu::Log("[Hook] GetFullName 收到 null 对象 (第 %d 次) — 已短路 (孤儿 World 校验报告)",
                         s_NullFullNameHits);
        }
        return OutFString;
    }
    return o_GetFullNameLike(This, OutFString);
}

// 游戏版本特定 RVA: GetFullName 风格函数 (UE 5.0.2 Shipping)
static constexpr uintptr_t RVA_GetFullNameLike = 0x17B9CB0;

static void InstallCrashPatches()
{
    if (!g_GameBase)
        return;
    void* target = (void*)(g_GameBase + RVA_GetFullNameLike);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hk_GetFullNameLike),
                       reinterpret_cast<void**>(&o_GetFullNameLike)) != MH_OK
        || MH_EnableHook(target) != MH_OK)
    {
        ModMenu::Log("[Hook] GetFullName 止血 hook 安装失败 (+0x%llX)!",
                     (unsigned long long)RVA_GetFullNameLike);
        o_GetFullNameLike = nullptr;
        return;
    }
    ModMenu::Log("[Hook] GetFullName 止血 hook 已安装 (+0x%llX)",
                 (unsigned long long)RVA_GetFullNameLike);
}

// ============ ProcessEvent 功能拦截 hook (EXE+0x18240D0) ============
// 重要前提 (2026-09-25 三轮实测后确认): ProcessEvent 只看得见【外部调用】——
// 引擎/原生代码去调 BP 函数 (Tick、动画、时间轴委托、重叠委托、UMG、感知回调、委托广播),
// 以及我们自己通过 SDK 桩函数发起的调用。**BP 之间互相调用不走这里**
// (VM 直接派发), 所以任何"按名字拦 BP 函数"的方案都拦不到 BP→BP 的调用。
// 职责 (全部为功能性拦截, 无诊断):
//   1) 上帝模式: 吞掉玩家伤害/死亡事件
//   2) 上帝模式: 玩家 ReceiveTick 入口清蜂蜇/受伤/死亡/重开/PlayerDead 终态位
//   3) 上帝模式: 修女处决链可见入口全名单 (BeginPlayerDeathEvent/ForceDeath/锤击)
//   4) 上帝模式: 玩家死亡时间轴冻结 (DeathTimeline__UpdateFunc/FinishedFunc —
//      任何来源的死亡序列都推进不了, BP→BP 不可见链路的终极兜底)
//   5) 修女失明: 吞掉 Philippa 的 ReceiveTick
//   6) 穿墙飞行: 禁用丢弃 (空格输入层兜底才真正生效)
// (2026-10-01: 藏身保护族 — 修女开柜拦截/柜门时间轴冻结/搜查列表摘除/检测盒重叠
//  拦截/出柜放行窗口/藏身字段守卫 — 已按用户要求整体移除)
using ProcessEvent_t = void (*)(SDK::UObject*, SDK::UFunction*, void*);
static ProcessEvent_t o_ProcessEvent = nullptr;

// 上帝模式伤害拦截 (定义在 RVA_ProcessEvent 之后): 返回 true = 玩家伤害事件, 吞掉
static bool GodModeShouldBlockDamage(SDK::UObject* This, SDK::UFunction* Func);

// (层 5 修女处决链拦截已并入 GodModeShouldBlockDamage 的修女侧名单 —
//  旧版只声明未实现, 且名单缺 ForceDeath/锤击入口, 现已补全)

// 穿墙飞行拦截 (定义在 RVA_ProcessEvent 之后): 返回 true = 该调用应被吞掉
static bool NoclipShouldBlockDrop(SDK::UObject* This, SDK::UFunction* Func);
static bool NoclipShouldBlockInput(SDK::UObject* This, SDK::UFunction* Func, void* Params);

// 处决链残局窗口: 层5刚拦到处决链入口 (拉出已发生) 的 2 秒内 —
// 残局由 ReceiveTick 侧的清理逻辑统一收拾 (清残留假藏身状态位 + 复位修女追捕),
// 死亡相机接管调用也在此窗口内一并吞掉
static bool CatchJustBlocked()
{
    return ModMenu::g_CatchBlockedTick != 0
        && GetTickCount64() - ModMenu::g_CatchBlockedTick <= 2000;
}

static void hk_ProcessEvent(SDK::UObject* This, SDK::UFunction* Func, void* Params)
{
    // 上帝模式: 吞掉玩家的伤害事件 (TakeDamage = 一切非摔落伤害的 BP 入口;
    // 修女击杀/摔落由 ApplyFeatures 的字段级开关覆盖)。吞掉 = 伤害与死亡流程完全不执行
    if (Menu::IsGodMode() && GodModeShouldBlockDamage(This, Func))
        return;

    // ---- 层S: 修女 BT 装饰器 CheckNunAIState_C::PerformConditionCheckAI ----
    // 游戏"当面进柜会被抓出 / 不当面被忽略"的判定最终落在 BT 分支门控上:
    // 装饰器参数表实证 (dumper): 内部就是 Cast<Madelaine>(ControlledPawn) +
    // EqualEqual_ByteByte(Pawn.Nun_Ai_State == 装饰器.State), 每 30~80ms 被 BT 轮询。
    // 在这一个钩子里完成: 真身缓存 / 状态变化日志 (枚举映射取证) / 修女压制 /
    // 装饰器配置采样。压制 (六测) = 上帝模式开启即全局把追捕态 2 抹平为 0,
    // 她因"看见玩家"而切换的追捕态被抹平 → 永远无法成为抓捕执行者。
    if (Menu::IsGodMode() && This && Func && Params && This->Class)
    {
        static SDK::UClass* s_ChkCls = nullptr;
        if (!s_ChkCls)
            s_ChkCls = SDK::UCheckNunAIState_C::StaticClass();
        if (s_ChkCls && This->Class == s_ChkCls)
        {
            static SDK::UFunction* s_ChkFn = nullptr;
            if (!s_ChkFn && Func->GetName() == "PerformConditionCheckAI")
                s_ChkFn = Func;
            if (Func == s_ChkFn)
            {
                // 参数表: OwnerController@0x0, ControlledPawn@0x8 (dumper 核对过)
                auto* Pawn = *(SDK::APawn**)((uint8_t*)Params + 8);
                if (Pawn && GameContext::IsUsable((SDK::UObject*)Pawn) && Pawn->Class)
                {
                    static SDK::UClass* s_MadClsS = nullptr;
                    if (!s_MadClsS)
                        s_MadClsS = SDK::ACharacter_Sister_Madelaine_Nun_C::StaticClass();
                    if (s_MadClsS && Pawn->Class == s_MadClsS)
                    {
                        auto* Nun = static_cast<SDK::ACharacter_Sister_Madelaine_Nun_C*>(Pawn);

                        // 状态变化日志 (实测枚举映射: 巡逻/追捕/抓捕各对应哪个值)
                        static unsigned s_PrevState = 0xFFFFFFFFu;
                        const unsigned CurState = (unsigned)Nun->Nun_Ai_State;
                        if (CurState != s_PrevState)
                        {
                            static int s_ChgLogs = 0;
                            if (s_ChgLogs < 40)
                            {
                                s_ChgLogs++;
                                char FromBuf[16];
                                if (s_PrevState == 0xFFFFFFFFu)
                                    snprintf(FromBuf, sizeof(FromBuf), "首读");
                                else
                                    snprintf(FromBuf, sizeof(FromBuf), "%u", s_PrevState);
                                ModMenu::Log("[AI] 修女状态机: %s→%u (装饰器.State=%u)",
                                             FromBuf, CurState,
                                             (unsigned)((SDK::UCheckNunAIState_C*)This)->State);
                            }
                            s_PrevState = CurState;
                        }

                        // 装饰器实例配置采样 (每实例一次 → BT 分支结构全貌)
                        static void* s_DecoSeen[8] = {};
                        bool Seen = false;
                        int Slot = -1;
                        for (int i = 0; i < 8; i++)
                        {
                            if (s_DecoSeen[i] == This) { Seen = true; break; }
                            if (!s_DecoSeen[i] && Slot < 0) Slot = i;
                        }
                        if (!Seen && Slot >= 0)
                        {
                            s_DecoSeen[Slot] = This;
                            ModMenu::Log("[AI] 装饰器#%d 配置 State=%u",
                                         Slot, (unsigned)((SDK::UCheckNunAIState_C*)This)->State);
                        }
                    }
                }
            }
        }
    }

    // 玩家 ReceiveTick (每帧一次):
    //   上帝模式: 在 Tick 入口清 Bees_Damage/BeingHurt/DeathActivated/ReestartGame
    //      (蜂蜇这类由状态位直接驱动的伤害没有可拦截的函数调用; Present 侧清标志
    //       晚于游戏 Tick, 必须在 Tick 入口清)
    if (This && This->Class && Func)
    {
        static SDK::UClass* s_PlyCls3 = nullptr;
        if (!s_PlyCls3)
            s_PlyCls3 = SDK::AEvilNun_Player_Character_C::StaticClass();
        static SDK::UFunction* s_ReceiveTickFn = nullptr;
        if (s_PlyCls3 && This->Class == s_PlyCls3)
        {
            if (!s_ReceiveTickFn && Func->GetName() == "ReceiveTick")
                s_ReceiveTickFn = Func;
            if (Func == s_ReceiveTickFn)
            {
                auto* Ply = static_cast<SDK::AEvilNun_Player_Character_C*>(This);

                if (Menu::IsGodMode())
                {
                    // Bees_Damage: 蜂蜇状态位; BeingHurt: 受伤状态 (呼吸回血系统);
                    // DeathActivated: 死亡序列开关; ReestartGame: 重开标志;
                    // PlayerDead: 玩家死亡终态位 (处决链 BP→BP 内部直写的最后一站,
                    // 旧版没清 — 修女穿柜杀时它一旦置位, 死亡 UI/重生流程照样走);
                    // Death_Cinematic: 死亡过场开关。
                    // (两字段定义在父类 NinaFirstPersonCharacter_C, 玩家类公开继承;
                    //  PlayerDead/Death_Cinematic 在玩家类自身)
                    // 死亡取证 (2026-10-01): 帧头非零 = 上一帧 BP 在我们清零后又写回 —
                    // 据此定位真正驱动死亡/复活的字段 (祭坛 BP→BP 直调零日志, 只能
                    // 靠字段回写反推)。诊断稳定后删除。
                    // 死亡字段每帧清零 ( BeingHurt/PlayerDead/ReestartGame 等状态位
                    // 由 BP 直写, 清零可阻断部分死亡 UI 流程)
                    Ply->Bees_Damage = false;
                    Ply->BeingHurt = false;
                    Ply->DeathActivated = false;
                    Ply->ReestartGame = false;
                    Ply->PlayerDead = false;
                    Ply->Death_Cinematic = false;

                    // ---- 处决链残局清理 (2026-09-30) ----
                    // 层5拦到处决链入口 = 玩家已被 BP→BP 不可见调用拉出柜。清掉残留的
                    // 假藏身状态位, 并复位修女追捕状态 (渲染线程消费标志)。
                    if (CatchJustBlocked())
                    {
                        static unsigned long long s_CleanedTick = 0;
                        if (s_CleanedTick != ModMenu::g_CatchBlockedTick)
                        {
                            s_CleanedTick = ModMenu::g_CatchBlockedTick;
                            Ply->PerformingHideAnimation = false;
                            Ply->HiddenInActor = nullptr;
                            // 修女追捕状态复位改由渲染线程执行 (menu.cpp 消费该标志) —
                            // GetNuns() 的静态 vector 由渲染线程重建, 游戏线程迭代会崩溃
                            ModMenu::g_CatchNunResetPending = true;
                            ModMenu::Log("[God] 处决链已断, 清理残局: 退出假藏身状态");
                        }
                    }
                }
            }
        }
    }

    // 失明: 吞掉 Philippa 的 ReceiveTick (检测逻辑全在 Tick 字节码内, 无法字段级拦截)
    if (ModMenu::g_BlockPhilippaTick && This && Func && This->Class)
    {
        static SDK::UClass* s_PhiBlk = SDK::ANun_Philippa_Character_Fat_chubby_C::StaticClass();
        if (This->Class == s_PhiBlk)
        {
            // 缓存 ReceiveTick 的 UFunction 指针: 之后纯指针比较, 零字符串开销
            static SDK::UFunction* s_TickFn = nullptr;
            if (!s_TickFn && Func->GetName() == "ReceiveTick")
                s_TickFn = Func;
            if (Func == s_TickFn)
                return; // 不调原函数 — BP 检测/呼叫逻辑完全不执行
        }
    }

    // 穿墙飞行: 禁用丢弃。用户把空格绑成了丢弃键, 飞行中按空格上升会误触发
    // 丢弃手中物品。两层拦截 (只在飞行开启期间生效, 关闭飞行立即恢复):
    //   1) 丢弃执行链: 玩家/道具的 NinaLeaveObject、道具 DropObject/ReleaseObject
    //   2) 输入层兜底: 玩家身上由 空格 触发的输入事件整条吞掉
    // 拾取/交互/投掷/道具使用不受影响 (只拦放下语义的函数与上升键)
    if (Menu::IsNoclip() && This && Func)
    {
        const bool IsDrop = This->Class && NoclipShouldBlockDrop(This, Func);
        const bool IsFlyKeyInput = Params && NoclipShouldBlockInput(This, Func, Params);
        if (IsDrop || IsFlyKeyInput)
        {
            static int s_BlockedLogs = 0;
            if (s_BlockedLogs < 8)
            {
                s_BlockedLogs++;
                ModMenu::Log("[Noclip] 飞行期间拦截 %s (%s, 第 %d 次)",
                             IsDrop ? "丢弃执行" : "飞行键输入",
                             Func->GetName().c_str(), s_BlockedLogs);
            }
            return;
        }
    }
    o_ProcessEvent(This, Func, Params);
}

static constexpr uintptr_t RVA_ProcessEvent = 0x18240D0;

// ============ 穿墙飞行: 丢弃拦截 ============
// 实测 (2026-09-25): 只拦"玩家 NinaLeaveObject + 道具 DropObject"无效 —— 真正的丢弃
// 链是【玩家 BP 直接调用手里那个道具自己的 NinaLeaveObject】(放下逻辑实现在道具上),
// 道具的 DropObject 只是内部子步骤、未必走。道具类函数表确认 ANunGrabObject_C 与
// APhysGrabObject_C 都带 NinaLeaveObject, 所以两侧同名函数都要拦。
// 函数名足够特异 → 不按类筛选 (顺带覆盖任何子类), 只按名字拦。
// 命中后 UFunction 指针进缓存, 热路径只有指针比较 (名字匹配仅首次遇到做一次)。
static bool NoclipShouldBlockDrop(SDK::UObject* This, SDK::UFunction* Func)
{
    if (!This || !Func || !This->Class)
        return false;
    // 廉价前置过滤: 只有玩家 / 任务道具 / 物理杂物三类对象需要继续看函数名
    // (ProcessEvent 每帧对全场景对象都会触发, 不能用字符串匹配当第一道门)
    static SDK::UClass* s_PlyCls  = nullptr;
    static SDK::UClass* s_GrabCls = nullptr;
    static SDK::UClass* s_PhysCls = nullptr;
    if (!s_PlyCls)  s_PlyCls  = SDK::AEvilNun_Player_Character_C::StaticClass();
    if (!s_GrabCls) s_GrabCls = SDK::ANunGrabObject_C::StaticClass();
    if (!s_PhysCls) s_PhysCls = SDK::APhysGrabObject_C::StaticClass();
    if (!(s_PlyCls && This->IsA(s_PlyCls)) && !(s_GrabCls && This->IsA(s_GrabCls))
        && !(s_PhysCls && This->IsA(s_PhysCls)))
        return false;

    static SDK::UFunction* s_Blocked[32] = {};
    static SDK::UFunction* s_Ignored[160] = {};
    for (auto* F : s_Blocked)
        if (F == Func) return true;
    for (auto* F : s_Ignored)
        if (F == Func) return false;

    const std::string N = Func->GetName();
    const bool Match = N == "NinaLeaveObject"   // 放下入口 (玩家/道具同名, 两侧都要拦)
                    || N == "DropObject"        // ANunGrabObject_C 实际放下
                    || N == "FakeDropObject"    // ANunGrabObject_C 脚本假放下
                    || N == "ReleaseObject";    // APhysGrabObject_C 放开
    if (Match)
    {
        for (auto*& F : s_Blocked)
            if (!F) { F = Func; break; }
    }
    else
    {
        for (auto*& F : s_Ignored)
            if (!F) { F = Func; break; }
    }
    return Match; // 缓存满也按匹配结果处理, 正确性不受影响
}

// ============ 穿墙飞行: 输入拦截 (兜底层) ============
// 即使丢弃链里还有没识别出的入口, 这里也拦得住: 飞行期间把玩家 "空格" 触发的输入
// 事件整条吞掉 — 空格是飞行上升键, 同时又被用户绑成了游戏里的丢弃键, 不能让同一次
// 按键既上升又丢东西。与游戏原生行为同一思路: 过场动画里快捷键提示照常显示, 但操作
// 无效 = 输入层门控。
// 只拦空格, 不拦下降键 (Shift): 下降是 mod 用 Win32 直接读键驱动的, 与游戏的输入映射
// 无关; 而 Shift 通常是游戏的奔跑键, 把它整条吞掉可能在玩家松手时把奔跑状态卡住 —
// 拦不必要的东西只会引入副作用。
// 输入事件签名都是 (const FKey& Key), Key 在参数块偏移 0 (见 dumper 参数表);
// FKey 首成员即 FName KeyName (0x0)。
static bool IsInputEventFunc(SDK::UFunction* Func)
{
    static SDK::UFunction* s_Fn[96] = {};
    static bool s_IsEvt[96] = {};
    for (int i = 0; i < 96; i++)
        if (s_Fn[i] == Func) return s_IsEvt[i];
    const std::string N = Func->GetName();
    const bool Evt = N.rfind("InpActEvt_", 0) == 0 || N.rfind("InpKeyEvt_", 0) == 0;
    for (int i = 0; i < 96; i++)
        if (!s_Fn[i]) { s_Fn[i] = Func; s_IsEvt[i] = Evt; break; }
    return Evt;
}

static bool NoclipShouldBlockInput(SDK::UObject* This, SDK::UFunction* Func, void* Params)
{
    if (!This || !Func || !This->Class || !Params)
        return false;
    static SDK::UClass* s_PlyCls = nullptr;
    if (!s_PlyCls)
        s_PlyCls = SDK::AEvilNun_Player_Character_C::StaticClass();
    if (!s_PlyCls || !This->IsA(s_PlyCls))
        return false;                 // 类比较最便宜, 先滤掉绝大多数对象调用
    if (!IsInputEventFunc(Func))
        return false;                 // 非输入事件 (Tick 等高频函数) 不做键名解析

    const std::string Key = ((SDK::FKey*)Params)->KeyName.ToString();
    return Key == "SpaceBar";         // 飞行上升键 = 丢弃键, 必须吞掉
}

// ============ 上帝模式: 免疫所有伤害 + 修女打不了 ============
// ① 免疫所有伤害 — 玩家一切伤害/死亡入口吞掉 (见下方"伤害入口白名单";
//    蜂群走玩家类 BeeHatIsOn 字段级免疫, 不在此处)
// ② 修女打不了 — 处决链入口吞掉 + 玩家死亡时间轴冻结 (任何来源的死亡
//    演出都推进不了, 见"死亡时间轴"块)
// 防卡配套: 处决链被拦后 2 秒残局窗口内吞掉死亡相机接管 (防视角锁死);
// 残局清理 (清玩家死亡位 + 修女复位) 在 Tick/渲染线程, 入口见 CatchJustBlocked
// 返回 true = 吞掉该调用 (不执行原函数)
static bool GodModeShouldBlockDamage(SDK::UObject* This, SDK::UFunction* Func)
{
    // ---- 防卡: 死亡相机接管拦截 (处决链被拦后游戏仍会切死亡相机/断输入) ----
    if (CatchJustBlocked())
    {
        // 函数指针缓存: 命中过的相机函数直接吞, 免对每次调用做 GetName 字符串分配
        static SDK::UFunction* s_CamFns[8] = {};
        bool CamKnown = false;
        for (auto& F : s_CamFns)
            if (F == Func) { CamKnown = true; break; }
        if (CamKnown)
            return true;
        const std::string CN = Func->GetName();
        const bool IsCamTakeover =
            CN == "SetViewTargetWithBlend" || CN == "SetViewTarget"
         || CN == "DisableInput" || CN == "SetIgnoreLookInput"
         || CN == "SetIgnoreMoveInput" || CN == "SetIgnoreActionInput"
         || CN == "SetConsumeInput";
        if (IsCamTakeover)
        {
            for (auto& F : s_CamFns)
                if (!F) { F = Func; break; }
            static int s_CamLogs = 0;
            if (s_CamLogs < 6)
                ModMenu::Log("[God] 拦截死亡相机接管 %s", CN.c_str());
            return true;
        }
    }

    // ---- 规则②: 修女处决链入口 (死亡演出/强制死亡/两种锤击) 一律吞掉 ----
    if (This && This->Class && Func)
    {
        // 只看修女类; 命中过的函数进缓存分流: s_NunKill=吞, s_NunOther=放行
        // (修女函数高频调用, 缓存避免对非处决函数反复 GetName)
        static SDK::UClass* s_MadCls = nullptr;
        if (!s_MadCls)
            s_MadCls = SDK::ACharacter_Sister_Madelaine_Nun_C::StaticClass();
        if (s_MadCls && This->Class == s_MadCls)
        {
            static SDK::UFunction* s_NunKill[8] = {};
            static SDK::UFunction* s_NunOther[160] = {};
            for (auto* F : s_NunKill)
                if (F == Func) return true;
            for (auto* F : s_NunOther)
                if (F == Func) return false;
            const std::string N = Func->GetName();
            const bool Match = N == "BeginPlayerDeathEvent"
                            || N == "ForceDeath"
                            || N == "PlayerHammerImpact"
                            || N == "SecurityHammerImpact";
            if (Match)
            {
                for (auto*& F : s_NunKill)
                    if (!F) { F = Func; break; }
                // 处决链被拦 → 开启 2 秒残局窗口 (相机拦截/修女复位/字段清理按它触发)
                ModMenu::g_CatchBlockedTick = GetTickCount64();
                static int s_ExecBlocked = 0;
                ModMenu::Log("[God] 拦截修女处决 %s (第 %d 次)", N.c_str(), ++s_ExecBlocked);
                return true;
            }
            for (auto*& F : s_NunOther)
                if (!F) { F = Func; break; }
            return false;
        }
    }

    // ---- 规则①/② 玩家侧入口: 非玩家类直接放行 ----
    static SDK::UClass* s_PlyCls = nullptr;
    if (!s_PlyCls)
        s_PlyCls = SDK::AEvilNun_Player_Character_C::StaticClass();
    if (!s_PlyCls || !This || This->Class != s_PlyCls || !Func)
        return false;

    // ---- 规则②兜底: 冻结玩家死亡时间轴 ----
    // 一切死亡演出 (修女/蜂群/摔落/不可见 BP 链) 全靠玩家身上 DeathTimeline
    // 每帧向 BP 广播 __UpdateFunc 推进 — 冻结它 = BP 内部写进来的死亡标志位
    // 被 Tick 清零后, 死亡流程再无第二条推进通道
    {
        static SDK::UFunction* s_DeathTl[4] = {};
        static SDK::UFunction* s_NotDeathTl[96] = {};
        bool Hit = false;
        for (auto* F : s_DeathTl)
            if (F == Func) { Hit = true; break; }
        if (!Hit)
        {
            bool Miss = false;
            for (auto* F : s_NotDeathTl)
                if (F == Func) { Miss = true; break; }
            if (!Miss)
            {
                if (Func->GetName().rfind("DeathTimeline", 0) == 0)
                {
                    for (auto*& F : s_DeathTl)
                        if (!F) { F = Func; break; }
                    Hit = true;
                }
                else
                {
                    for (auto*& F : s_NotDeathTl)
                        if (!F) { F = Func; break; }
                }
            }
        }
        if (Hit)
        {
            static int s_DtlLogs = 0;
            if (s_DtlLogs < 8)
            {
                s_DtlLogs++;
                ModMenu::Log("[God] 冻结玩家死亡时间轴 %s (第 %d 次)",
                             Func->GetName().c_str(), s_DtlLogs);
            }
            return true;
        }
    }

    // ---- 规则①: 玩家伤害入口白名单 (TakeDamage/KillPlayer/任意·点·径向伤害/
    //      摔出世界/落地; 缓存防重复 GetName) ----
    static SDK::UFunction* s_Known[64] = {};
    bool Known = false;
    for (auto& F : s_Known)
    {
        if (F == Func) { Known = true; break; }
    }
    if (!Known)
    {
        const std::string N = Func->GetName();
        bool Match = N == "TakeDamage" || N == "KillPlayer" || N == "DeathEvent"
                  || N == "ReceiveAnyDamage" || N == "ReceivePointDamage"
                  || N == "ReceiveRadialDamage"
                  || N == "FellOutOfWorld" || N == "OutsideWorldBounds"
                  || N == "OnLanded" || N == "Landed";
        if (!Match)
            return false;
        for (auto& F : s_Known)
            if (!F) { F = Func; Known = true; break; }
        if (!Known)
            return false;
    }

    static int s_Blocked = 0;
    s_Blocked++;
    ModMenu::Log("[God] 拦截 %s (第 %d 次)", Func->GetName().c_str(), s_Blocked);
    return true;
}

// ============ GC 校验器前置清扫 hook (EXE+0x3C3E5A0) ============
// 根治结算/返回主菜单的 GC Fatal: 游戏销毁 async 对象到 GC 校验的间隙可能 < 500ms,
// 周期清扫存在时间盲区 (上轮 Fatal: BodySetup 在销毁后立即被校验)。
// 本函数是 Fatal 的直接来源 (反汇编确认: 遍历所有 UWorld 找"未清理干净的旧 World"),
// 在它入口先全量清扫 async+PendingKill 僵尸 → 校验器看到的世界必然干净, 零盲区。
// x64 前 4 参经寄存器, 转发声明 4 个即可 (多余栈参数由 MinHook trampoline 保留)
using WorldGCAuditor_t = void* (*)(void*, void*, void*, void*);
static WorldGCAuditor_t o_WorldGCAuditor = nullptr;
static int s_AuditorSweeps = 0;

static void* hk_WorldGCAuditor(void* a, void* b, void* c, void* d)
{
    // 游戏线程同步调用 (校验器由主线程触发), 此处清扫无竞争
    GameContext::SweepZombieAsyncFlags("GC校验前");
    // 旧 World/Level 自身的 async 位也要清 (async=GC root, 带位则无引用也不销毁)。
    // 保守模式 (IncludeCurrent=false): 校验器运行在 GC 之后, 被校验的"旧 World"必然
    // 已脱离 WorldContext (校验器本身跳过当前 World), 走非当前路径即可覆盖;
    // 当前/加载中的 World 绝不能碰 — 其 async 位是加载期的合法保护, 清了会让 GC
    // 收走半成品 World → 加载器 AV (2026-08-29)。
    GameContext::ClearOldWorldAsyncFlags(false);
    if (s_AuditorSweeps < 3)
    {
        s_AuditorSweeps++;
        ModMenu::Log("[Hook] GC 校验器运行 (第 %d 次观察到) — 已前置清扫", s_AuditorSweeps);
    }
    return o_WorldGCAuditor(a, b, c, d);
}

static void InstallGCAuditorHook()
{
    if (!g_GameBase)
        return;
    void* target = (void*)(g_GameBase + 0x3C3E5A0);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hk_WorldGCAuditor),
                       reinterpret_cast<void**>(&o_WorldGCAuditor)) != MH_OK
        || MH_EnableHook(target) != MH_OK)
    {
        ModMenu::Log("[Hook] GC 校验器 hook 安装失败 (+0x3C3E5A0)!");
        o_WorldGCAuditor = nullptr;
        return;
    }
    ModMenu::Log("[Hook] GC 校验器 hook 已安装 (+0x3C3E5A0)");
}

static void InstallProcessEventHook()
{
    if (!g_GameBase)
        return;
    void* target = (void*)(g_GameBase + RVA_ProcessEvent);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hk_ProcessEvent),
                       reinterpret_cast<void**>(&o_ProcessEvent)) != MH_OK
        || MH_EnableHook(target) != MH_OK)
    {
        ModMenu::Log("[Hook] ProcessEvent 拦截 hook 安装失败 (+0x%llX)!",
                     (unsigned long long)RVA_ProcessEvent);
        o_ProcessEvent = nullptr;
        return;
    }
    ModMenu::Log("[Hook] ProcessEvent 拦截 hook 已安装 (+0x%llX)",
                 (unsigned long long)RVA_ProcessEvent);
}

// ============ 崩溃诊断 (VEH): 崩溃瞬间把出错地址/寄存器写入日志 ============
static void* g_VehHandle = nullptr;

// 模块快照: 把任意地址解析成 "模块名+RVA" (只认游戏EXE和本DLL时大量地址无法定位)
struct ModInfo { uintptr_t Base; uintptr_t Size; char Name[64]; };
static std::vector<ModInfo> s_Modules;

static void SnapshotModules()
{
    s_Modules.clear();
    HMODULE mods[512];
    DWORD cb = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &cb))
        return;
    int n = (int)(cb / sizeof(HMODULE));
    if (n > 512) n = 512;
    for (int i = 0; i < n; i++)
    {
        wchar_t wname[MAX_PATH] = L"?";
        GetModuleBaseNameW(GetCurrentProcess(), mods[i], wname, MAX_PATH);
        ModInfo mi{};
        mi.Base = (uintptr_t)mods[i];
        auto* dos = (IMAGE_DOS_HEADER*)mods[i];
        if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)mods[i] + dos->e_lfanew);
            if (nt && nt->Signature == IMAGE_NT_SIGNATURE)
                mi.Size = nt->OptionalHeader.SizeOfImage;
        }
        if (!mi.Size) continue;
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, mi.Name, sizeof(mi.Name), nullptr, nullptr);
        s_Modules.push_back(mi);
    }
    ModMenu::Log("[Crash] 模块快照: %d 个", (int)s_Modules.size());
}

// 把地址解析为静态缓冲 (日志用), 找不到返回 "其他"
static const char* ResolveAddr(uintptr_t v)
{
    static char Buf[96];
    for (const auto& m : s_Modules)
    {
        if (v >= m.Base && v < m.Base + m.Size)
        {
            snprintf(Buf, sizeof(Buf), "%s+0x%llX", m.Name, (unsigned long long)(v - m.Base));
            return Buf;
        }
    }
    return "其他";
}

// 崩溃时刻的寄存器/栈值反查 GObjects: 若该地址是(或曾是)引擎对象, 输出其名/类/销毁状态
// — 悬垂引用崩溃的关键证据: "读到的是哪个对象"
static void ResolveUObjectCandidates(const CONTEXT* c)
{
    auto* Arr = SDK::UObject::GObjects.GetTypedPtr();
    if (!Arr)
        return;
    uintptr_t Vals[10] = { c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbx, c->Rax, c->R8, c->R9, c->Rbp, c->R10 };
    const int Num = Arr->Num();
    for (uintptr_t V : Vals)
    {
        if (V < 0x10000)
            continue;
        for (int i = 0; i < Num; i++)
        {
            SDK::UObject* O = Arr->GetByIndex(i);
            if ((uintptr_t)O != V)
                continue;
            // 对象仍在册 — 安全读名字/类 (VEH 时游戏线程已停, 无并发问题)
            const char* Cls = O->Class ? O->Class->GetName().c_str() : "?";
            bool Destroyed = ((uint32_t)O->Flags & ((uint32_t)SDK::EObjectFlags::BeginDestroyed
                                                   | (uint32_t)SDK::EObjectFlags::FinishDestroyed)) != 0;
            ModMenu::Log("[Crash]   %p = UObject [%s / %s]%s",
                         (void*)V, O->GetName().c_str(), Cls,
                         Destroyed ? " (销毁中!)" : "");
        }
    }
}

static LONG WINAPI CrashDiagHandler(EXCEPTION_POINTERS* ep)
{
    const auto* er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        const uintptr_t eip = (uintptr_t)er->ExceptionAddress;
        const uintptr_t acc = er->ExceptionInformation[1];
        const CONTEXT* c = ep->ContextRecord;
        ModMenu::Log("[Crash] AV %s地址 %p, 出错指令 %s, 线程 %u",
                     er->ExceptionInformation[0] ? "写" : "读", (void*)acc,
                     ResolveAddr(eip), GetCurrentThreadId());
        ModMenu::Log("[Crash] RCX=%p RDX=%p R8=%p R9=%p RAX=%p",
                     (void*)c->Rcx, (void*)c->Rdx, (void*)c->R8,
                     (void*)c->R9, (void*)c->Rax);
        ModMenu::Log("[Crash] RBX=%p RSI=%p RDI=%p RBP=%p RSP=%p",
                     (void*)c->Rbx, (void*)c->Rsi, (void*)c->Rdi,
                     (void*)c->Rbp, (void*)c->Rsp);
        // 寄存器值反查 GObjects (悬垂对象定位的关键证据)
        ResolveUObjectCandidates(c);
        // 栈顶若干 qword (常见为崩溃时的调用链返回地址)
        // MinGW 无 __try/__except, 用 IsBadReadPtr 防御不可读页
        const uintptr_t* sp = (const uintptr_t*)c->Rsp;
        for (int i = 0; i < 12; i++)
        {
            if (IsBadReadPtr(&sp[i], sizeof(uintptr_t)))
                break;
            const uintptr_t v = sp[i];
            ModMenu::Log("[Crash]   栈[%d] = %p (%s)", i, (void*)v, ResolveAddr(v));
        }
    }
    return EXCEPTION_CONTINUE_SEARCH; // 交给 UE 自己的崩溃处理 (弹窗)
}

static DWORD WINAPI MainThread(LPVOID)
{
    ModMenu::Log("EvilNunModMenu 已注入 (版本 %s), 正在初始化...", ModMenu::GetBuildStamp());

    // 解析游戏 EXE 基址/大小 (崩溃时把地址换算成 RVA 定位出错模块)
    {
        auto* base = (uint8_t*)GetModuleHandleW(nullptr);
        if (base)
        {
            auto* dos = (IMAGE_DOS_HEADER*)base;
            if (dos->e_magic == IMAGE_DOS_SIGNATURE)
            {
                auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE)
                {
                    g_GameBase = (uintptr_t)base;
                    g_GameSize = nt->OptionalHeader.SizeOfImage;
                    ModMenu::Log("[Crash] 游戏基址 %p (大小 0x%zX)", (void*)g_GameBase, g_GameSize);
                }
            }
        }
    }
    SnapshotModules();
    g_VehHandle = AddVectoredExceptionHandler(1, CrashDiagHandler);

    // 初始化渲染 hook (DXGI Present)
    if (!Renderer::InitHooks())
    {
        ModMenu::Log("错误: 渲染 hook 初始化失败!");
        FreeLibraryAndExitThread(g_hModule, 1);
        return 1;
    }
    ModMenu::Log("Hook 安装完成, 等待游戏调用 Present...");

    // 结算崩溃止血 hook (须在 MinHook 初始化后安装)
    InstallCrashPatches();
    // ProcessEvent 功能拦截 hook (上帝模式伤害/失明/飞行禁丢弃)
    InstallProcessEventHook();
    // GC 校验器前置清扫 hook (根治结算 Fatal)
    InstallGCAuditorHook();

    // 等待卸载信号 (END 键卸载 DLL)
    // 看门狗: 诊断 Present 是否被调用
    int Ticks = 0;
    bool Warned = false;
    while (!(GetAsyncKeyState(VK_END) & 0x8000))
    {
        if (!Warned && ++Ticks >= 50) // 5 秒
        {
            Warned = true;
            if (!ModMenu::g_Initialized)
                ModMenu::Log("警告: 5 秒内 Present 未被调用! 游戏可能使用 Vulkan/OpenGL 渲染, "
                             "或 dxgi.dll 的 Present 与游戏实际调用的不是同一实现");
            else
                ModMenu::Log("渲染已就绪 (ImGui 初始化成功)");
        }
        Sleep(100);
    }

    ModMenu::Log("正在卸载...");
    if (g_VehHandle)
    {
        RemoveVectoredExceptionHandler(g_VehHandle);
        g_VehHandle = nullptr;
    }
    Renderer::Shutdown();
    Sleep(300); // 等所有线程彻底离开 DLL 代码

    FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;
        HANDLE hThread = CreateThread(nullptr, 0, MainThread, nullptr, 0, &g_MainThreadId);
        if (hThread)
            CloseHandle(hThread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        Renderer::Shutdown();
    }
    return TRUE;
}
