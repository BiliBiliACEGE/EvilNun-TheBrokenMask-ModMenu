#include "menu.h"
#include "common.h"
#include "game_context.h"
#include "lang.h"
#include "imgui.h"

#include <SDK.hpp>
#include <cmath>
#include <cctype>
#include <cwchar>
#include <unordered_map>

namespace Menu {

// ============ 功能开关 ============
static bool s_GodMode = false;
static bool s_InfiniteStamina = false;
static bool s_FreezeNuns = false;
static bool s_NunsBlind = false;
static bool s_NunsDeaf = false;
static bool s_EspNuns = false;
static bool s_EspDistance = false;
static bool s_NunScaleEnabled = false;

// 修女击杀/死亡时间控制 (按单个修女粒度)
static bool s_NunsStayDead = false;      // 永久死亡 (死亡压制不自动解除)
static float s_NunDeadSeconds = 30.f;    // 死亡持续时间 (秒), 硬上限 = 游戏自身复活耗时
struct NunDeadHold                        // 单个修女的死亡压制记录
{
    SDK::ACharacter* Nun;
    ULONGLONG UntilTick;                 // 到期时刻 (ULLONG_MAX=永久)
    ULONGLONG KillTick = 0;              // 本次击杀时刻 (用于观测游戏自身复活耗时)
    ULONGLONG ReviveSeenTick = 0;        // 首次观察到"已复活"的时刻 (起身稳定期计时)
    bool ForceEarly = false;             // 到期时清死亡标志让她提前起身 (时长 < 30s 时)
};
static std::vector<NunDeadHold> s_DeadHolds;

// ============ 游戏自身复活耗时 = 30 秒 (2026-09-25 确认) ============
// 修女被击杀后, 游戏自己的复活流程固定 30 秒到点起身。这是"死亡持续时间"的硬上限:
// 设得更长毫无意义 (游戏到点就让她站起来), 旧版本允许设 30+ 秒正是"游戏复活→我们
// 补刀→再复活→再补刀"对抗的根源, 且补刀会落在起身演出中途把她卡成站着不动。
// 小于 30 秒 = 我们的到期清理提前把死亡标志清掉, 让她提前起身 (游戏不会这么做)。
static constexpr float     k_GameReviveSec = 30.f;
static constexpr ULONGLONG k_GameReviveMs  = 30000;

// 实测观测 (仅诊断/回归用, 不再参与钳制): 明显偏离 30 秒说明击杀落在演出中途,
// 或该关卡的时序特殊 — 记日志便于发现, 不改变上限。
static ULONGLONG s_GameReviveSeenMs    = 0; // 观测到的最小值
static ULONGLONG s_GameReviveSeenMaxMs = 0; // 观测到的最大值

// 记录一次实测值
static void NoteGameReviveMs(ULONGLONG Ms, const char* Who)
{
    if (Ms < 1000)
        return; // 状态抖动, 不算
    if (s_GameReviveSeenMs == 0 || Ms < s_GameReviveSeenMs)
        s_GameReviveSeenMs = Ms;
    if (Ms > s_GameReviveSeenMaxMs)
        s_GameReviveSeenMaxMs = Ms;
    // 偏离已知的 30 秒超过 3 秒才提示 (正常抖动不刷日志)
    if (Ms + 3000 < k_GameReviveMs || Ms > k_GameReviveMs + 3000)
        ModMenu::Log("[Nun] 注意: 实测 %s 复活耗时 ≈ %.1f 秒, 偏离已知的 %.0f 秒 "
                     "(可能本次击杀落在演出中途)", Who, Ms / 1000.0, k_GameReviveSec);
}

// 她是否还在播动画蒙太奇 (倒地/起身演出未结束)。此时补刀会打断她的复活状态机,
// 结果她站着却被死亡标志冻住 (原地不动) — 必须等演出彻底结束再动手
static bool NunPlayingMontage(SDK::ACharacter* Nun)
{
    if (!Nun || !Nun->Mesh)
        return false;
    auto* Anim = Nun->Mesh->GetAnimInstance();
    return Anim && Anim->IsAnyMontagePlaying();
}

// ============ 数值配置 ============
static float s_WalkSpeed = 200.f;
static float s_RunSpeed = 600.f;
static float s_CrouchSpeed = 150.f;
static float s_JumpHeight = 500.f;
static float s_NunSpeedOverride = 0.f;
static float s_FOV = 90.f;
static float s_Gamma = 2.2f;
static float s_NunScale = 1.0f;
static bool s_ModifySpeed = false;
static bool s_ModifyJump = false;
static bool s_ModifyFOV = false;
static bool s_ModifyGamma = false;
// FOV 关闭恢复状态
static bool s_FOVApplied = false;      // 本次开启期间真的改过 FOV

// 击杀流程本体 (NinaExplode 包装) — 定义在"修女操作"区, ApplyFeatures 的压制循环复用
static void NinaExplodeKill(SDK::ACharacter* Nun);

// 记录原始值用于恢复
static float s_OrgWalkSpeed = 0, s_OrgRunSpeed = 0, s_OrgCrouchSpeed = 0;
static bool s_SpeedOriginalSaved = false;
static double s_OrgJumpHeight = 0;
static float s_OrgJumpZVelocity = 0;
static bool s_OrgEnableJump = false;
static bool s_JumpOriginalSaved = false;
// 穿墙飞行
static bool s_Noclip = false;
static bool s_NoclipActive = false;
static float s_NoclipSpeed = 1000.f;
// 上帝模式地形救援
static SDK::FVector s_GodSafeLoc{ 0, 0, 0 };
static bool s_HasGodSafeLoc = false;

// ============ 功能关闭恢复系统 ============
// 根因: 部分开关只在开启时覆写游戏字段 (冻结/失明/失聪/击杀禁用/体力/上帝模式
// 字段/体型/FOV/Gamma), 关闭后没有恢复路径 — 覆写值留在对象里, 表现为"关了没
// 反应, 重启游戏才还原"。统一修法: 首次覆写前保存原始值, 关闭时恢复。
// 对象会换实例 (关卡切换/重生): 备份按 对象指针 + GObjects 索引 双重记录,
// 恢复前校验仍是同一对象; 失效则丢弃备份 (新实例天生是原始值, 无需恢复)。

// 无解引用的对象存活校验 (悬垂指针不可读, 只查 GObjects 槽位)
static bool ObjAliveAt(SDK::UObject* O, int32_t Index)
{
    if (!O || Index < 0)
        return false;
    auto* Arr = SDK::UObject::GObjects.GetTypedPtr();
    return Arr && Index < Arr->Num() && Arr->GetByIndex(Index) == O;
}

// 委托调用列表 Num 读/写 (TMulticastInlineDelegate = TArray 布局, Num 在 +8)
static int32_t DelegateNum(void* Delegate)
{
    return *(int32_t*)((uint8_t*)Delegate + 8);
}
static void SetDelegateNum(void* Delegate, int32_t Num)
{
    *(int32_t*)((uint8_t*)Delegate + 8) = Num;
}

// 玩家字段备份 (单实例; 玩家指针变化 = 换了 Pawn, 整体重置)
struct PlayerBackup
{
    SDK::AEvilNun_Player_Character_C* Player = nullptr;
    int32_t Index = -1;
    // 上帝模式
    bool HasGod = false;
    bool GodModeEnabled = false;
    bool NoFallDamage = false;
    int32_t DispatcherNum = -1;     // TakeDamageDispatcher 原始调用列表长度
    bool DispatcherRebound = false; // 开启期间引擎重绑过 (恢复计数不再安全, 交给引擎)
    int32_t RestartDispatcherNum = -1;     // ReestartGameDispatcher 原始调用列表长度
    bool RestartDispatcherRebound = false; // 同上 (死亡重开广播封口用)
    // 无限体力
    bool HasStamina = false;
    double RunTiredSpeed = 0;
};
static PlayerBackup s_PB;

// 修女字段备份 (每修女一份; map 键 = 修女指针, 值内含索引做身份校验)
struct NunBackup
{
    SDK::ACharacter* Nun = nullptr;
    int32_t Index = -1;
    // 冻结
    bool HasFreeze = false;
    double MaxNunSpeed = 0;
    float CMMaxWalkSpeed = 0;
    // 失明
    bool HasBlind = false;          // Mad: 视距字段
    double SightMaxDistance = 0;
    bool CustomSightEnabled = false;
    int32_t PlayerSeenNum = -1;     // Phi: PlayerSeen 委托原始长度 (<0 = 未记录)
    bool PlayerSeenRebound = false;
    // 失聪 (Mad)
    bool HasDeaf = false;
    double NunHearingDistance = 0;
    // 击杀禁用 (Mad, 上帝模式的一部分)
    bool HasKill = false;
    bool CanKillPlayer = false;
    bool CanPerformPlayerKill = false;
    // 抓捕距离封零 (Mad, 上帝模式的一部分)
    bool HasCatch = false;
    double CatchDistance = 0;
    double CrouchCatchDistance = 0;
    // 体型
    bool HasScale = false;
    SDK::FVector Scale{ 0, 0, 0 };
};
static std::unordered_map<SDK::ACharacter*, NunBackup> s_NunBk;

// 取修女字段备份 (仅当任一需要备份的功能开启时才建 map 条目 — 全关时返回哑备份,
// 避免主循环与恢复清理每帧互相插拔条目; 哑备份不会被任何功能块读到)
static NunBackup& NunBkFor(SDK::ACharacter* Nun)
{
    static NunBackup s_Dummy;
    if (!(s_FreezeNuns || s_NunsBlind || s_NunsDeaf || s_GodMode || s_NunScaleEnabled))
        return s_Dummy;
    auto& B = s_NunBk[Nun];
    if (B.Nun != Nun || B.Index != Nun->Index)
    {
        B = NunBackup{};
        B.Nun = Nun;
        B.Index = Nun->Index;
    }
    return B;
}

// ============ 每帧功能应用 ============

// AnyKey 输入事件桥 (dumper 只生成了父类 ANinaFirstPersonCharacter_C 的声明,
// 没生成 .cpp 实现 → 链接错): 手写 ProcessEvent 派发
static void AnyKeyEvt_Invoke(SDK::AEvilNun_Player_Character_C* P, const SDK::FKey& Key)
{
    static SDK::UFunction* Fn = nullptr;
    if (!Fn)
        Fn = P->Class->GetFunction(
            "NinaFirstPersonCharacter_C", "InpActEvt_AnyKey_K2Node_InputKeyEvent_7");
    if (!Fn)
        return;
    struct { SDK::FKey Key; }_Parms{ Key };
    P->ProcessEvent(Fn, &_Parms);
}

static void ApplyFeatures()
{
    auto* Player = GameContext::GetPlayer();
    auto* GI = GameContext::GetGameInstance();

    if (Player && GameContext::IsUsable(Player))
    {
        // 玩家备份身份校验: 换了 Pawn 实例 (关卡切换/重生) → 旧备份全部作废
        // (新实例天生是原始值), 整体重置
        if (s_PB.Player && s_PB.Player != Player)
            s_PB = PlayerBackup{};
        if (!s_PB.Player)
        {
            s_PB.Player = Player;
            s_PB.Index = Player->Index;
        }

        // 上帝模式 = 免伤 (BP 伤害入口由 dllmain ProcessEvent 拦截) + 免摔落
        //            + 修女无法击杀 (见下方修女循环)
        if (s_GodMode)
        {
            if (!s_PB.HasGod)
            {
                s_PB.GodModeEnabled = Player->GodModeEnabled;
                s_PB.NoFallDamage = Player->NoFallDamage;
                s_PB.DispatcherNum = DelegateNum(&Player->TakeDamageDispatcher);
                s_PB.DispatcherRebound = false;
                s_PB.RestartDispatcherNum = DelegateNum(&Player->ReestartGameDispatcher);
                s_PB.RestartDispatcherRebound = false;
                s_PB.HasGod = true;
            }
            Player->GodModeEnabled = true;
            Player->NoFallDamage = true;
            // 蜂群免疫 = 游戏自己的"养蜂人的帽子"判定标志 (玩家类 BeeHatIsOn @0x0E38,
            // BeeKeeperHat 组件 @0x0A20; 蜂群 AI 检查它决定是否蛰人)。直接置位 =
            // 与戴帽子等价的正规免疫路径, 不留备恢复 (关上帝模式后帽子仍在, 无副作用)
            Player->BeeHatIsOn = true;
            // 委托封口: TakeDamageDispatcher 的绑定处理器是引擎伤害路径的最后一环
            // (蜂群等外部伤害源 → 引擎 TakeDamage → 委托 → 任意名处理器 → 内联死亡,
            // 处理器名不可知无法拦截)。清空调用计数 = 广播变空操作
            // (与失明清 PlayerSeen 同法: 不释放 Data, 析构按 Max 释放, 无泄漏)
            // 清零前检测重绑: 引擎重绑过 = 数组内容已不是原布局, 恢复计数不安全
            if (DelegateNum(&Player->TakeDamageDispatcher) > 0)
                s_PB.DispatcherRebound = true;
            SetDelegateNum(&Player->TakeDamageDispatcher, 0);
            // ReestartGameDispatcher 封口: 处决链的 BP→BP 内部段可以在同一段图执行里
            // 直接置 ReestartGame=true 并立即广播重开 (标志位清零拦不住同帧广播),
            // 清空调用列表 = 重开广播变空操作 (2026-09-30 穿柜杀修复)
            if (DelegateNum(&Player->ReestartGameDispatcher) > 0)
                s_PB.RestartDispatcherRebound = true;
            SetDelegateNum(&Player->ReestartGameDispatcher, 0);
            // 受伤/蜂蜇/死亡/重开状态位每帧清零 (呼吸回血系统 BeingHurt + AutoHeal;
            // 即使某条伤害路径绕过拦截直接置位, 上帝模式下也进不了受伤/死亡流程)
            // PlayerDead/Death_Cinematic: 处决链 BP→BP 直写的死亡终态位, 一并清掉
            Player->BeingHurt = false;
            Player->Bees_Damage = false;
            Player->DeathActivated = false;
            Player->ReestartGame = false;
            Player->PlayerDead = false;
            Player->Death_Cinematic = false;
        }
        else if (s_PB.HasGod)
        {
            // 关闭恢复: 字段还原 + 委托计数还原 (引擎没重绑过才安全; 重绑过则
            // 保持现状, 引擎自己的绑定已生效)
            Player->GodModeEnabled = s_PB.GodModeEnabled;
            Player->NoFallDamage = s_PB.NoFallDamage;
            if (!s_PB.DispatcherRebound && s_PB.DispatcherNum > 0
                && DelegateNum(&Player->TakeDamageDispatcher) == 0)
                SetDelegateNum(&Player->TakeDamageDispatcher, s_PB.DispatcherNum);
            if (!s_PB.RestartDispatcherRebound && s_PB.RestartDispatcherNum > 0
                && DelegateNum(&Player->ReestartGameDispatcher) == 0)
                SetDelegateNum(&Player->ReestartGameDispatcher, s_PB.RestartDispatcherNum);
            s_PB.HasGod = false;
        }

        if (s_InfiniteStamina)
        {
            if (!s_PB.HasStamina)
            {
                s_PB.RunTiredSpeed = Player->RunTiredSpeed;
                s_PB.HasStamina = true;
            }
            Player->RunTime = 100.0;
            Player->ExaustedPlayer = false;
            Player->RunTiredSpeed = Player->MaxRunSpeed;
        }
        else if (s_PB.HasStamina)
        {
            // RunTime/ExaustedPlayer 游戏自己会恢复, 只有被覆写的疲劳速度需要还原
            Player->RunTiredSpeed = s_PB.RunTiredSpeed;
            s_PB.HasStamina = false;
        }

        // 速度/跳跃的备份同样做实例校验: 换 Pawn 后旧值不属于新实例, 直接丢弃
        // (新实例是原始速度, 硬套旧值反而可能错)
        static SDK::AEvilNun_Player_Character_C* s_SpeedSavedPlayer = nullptr;
        static int32_t s_SpeedSavedIndex = -1;
        if (s_SpeedOriginalSaved
            && (s_SpeedSavedPlayer != Player || s_SpeedSavedIndex != Player->Index))
            s_SpeedOriginalSaved = false;
        if (s_ModifySpeed)
        {
            if (!s_SpeedOriginalSaved)
            {
                s_OrgWalkSpeed = (float)Player->MaxWalkSpeed;
                s_OrgRunSpeed = (float)Player->MaxRunSpeed;
                s_OrgCrouchSpeed = (float)Player->MaxCrouchedSpeed;
                s_SpeedSavedPlayer = Player;
                s_SpeedSavedIndex = Player->Index;
                s_SpeedOriginalSaved = true;
            }
            Player->MaxWalkSpeed = s_WalkSpeed;
            Player->MaxRunSpeed = s_RunSpeed;
            Player->MaxCrouchedSpeed = s_CrouchSpeed;
            if (Player->CharacterMovement)
                Player->CharacterMovement->MaxWalkSpeed = s_WalkSpeed;
        }
        else if (s_SpeedOriginalSaved)
        {
            Player->MaxWalkSpeed = s_OrgWalkSpeed;
            Player->MaxRunSpeed = s_OrgRunSpeed;
            Player->MaxCrouchedSpeed = s_OrgCrouchSpeed;
            if (Player->CharacterMovement)
                Player->CharacterMovement->MaxWalkSpeed = s_OrgWalkSpeed;
            s_SpeedOriginalSaved = false;
        }

        static SDK::AEvilNun_Player_Character_C* s_JumpSavedPlayer = nullptr;
        static int32_t s_JumpSavedIndex = -1;
        if (s_JumpOriginalSaved
            && (s_JumpSavedPlayer != Player || s_JumpSavedIndex != Player->Index))
            s_JumpOriginalSaved = false;

        if (s_ModifyJump)
        {
            // 本游戏 (Evil Nun: The Broken Mask) 默认禁用跳跃: 框架 (Nina
            // FirstPersonCharacter) 有完整跳跃系统 (EnableJump 门控 + 输入事件
            // InpActEvt_Jump_*), 但本作把 EnableJump 关了 → 按空格无反应。
            // 解锁 = 置 EnableJump; 高度 = 写引擎 JumpZVelocity (跳跃输入走
            // ACharacter::Jump → 引擎按 JumpZVelocity 起跳, BP 的 JumpHeight
            // 只是存值, 跳跃流程不读它 — 三处一起写才完整生效)
            if (!s_JumpOriginalSaved)
            {
                s_OrgEnableJump = Player->EnableJump;
                s_OrgJumpHeight = Player->JumpHeight;
                if (Player->CharacterMovement)
                    s_OrgJumpZVelocity = Player->CharacterMovement->JumpZVelocity;
                s_JumpSavedPlayer = Player;
                s_JumpSavedIndex = Player->Index;
                s_JumpOriginalSaved = true;
                ModMenu::Log("[Player] 跳跃原始值: EnableJump=%d JumpHeight=%.1f JumpZVelocity=%.1f",
                             s_OrgEnableJump ? 1 : 0, s_OrgJumpHeight, s_OrgJumpZVelocity);
            }
            Player->EnableJump = true;
            Player->JumpHeight = s_JumpHeight;
            if (Player->CharacterMovement)
                Player->CharacterMovement->JumpZVelocity = s_JumpHeight;
        }
        else if (s_JumpOriginalSaved)
        {
            Player->EnableJump = s_OrgEnableJump;
            Player->JumpHeight = s_OrgJumpHeight;
            if (Player->CharacterMovement)
                Player->CharacterMovement->JumpZVelocity = s_OrgJumpZVelocity;
            s_JumpOriginalSaved = false;
        }

        // 穿墙飞行: Flying 移动模式 + 关闭玩家碰撞 → WASD 平飞穿墙;
        // 空格升 / Shift 降 (左或右 Shift 都行; 菜单输入框打字时不响应)
        if (s_Noclip)
        {
            if (Player->CharacterMovement)
            {
                Player->CharacterMovement->SetMovementMode(SDK::EMovementMode::MOVE_Flying, 0);
                Player->CharacterMovement->MaxFlySpeed = s_NoclipSpeed;
            }
            Player->SetActorEnableCollision(false);
            float Dt = ImGui::GetIO().DeltaTime;
            bool KeysOk = !ImGui::GetIO().WantCaptureKeyboard;
            bool Up = KeysOk && (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
            bool Down = KeysOk && ((GetAsyncKeyState(VK_LSHIFT) | GetAsyncKeyState(VK_RSHIFT)) & 0x8000) != 0;
            if (Dt > 0.f && Dt < 0.5f && (Up || Down))
            {
                double V = (double)s_NoclipSpeed * 1.5 * Dt;
                Player->K2_AddActorWorldOffset(
                    SDK::FVector{ 0, 0, Up ? V : -V }, false, nullptr, true);
            }
            // (G 键丢弃功能已移除 — 游戏丢弃链无法可靠重放)
            s_NoclipActive = true;
        }
        else if (s_NoclipActive)
        {
            // 关闭时恢复一次: 碰撞 + Walking (高空关闭会摔落 — 无上帝模式注意)
            if (Player->CharacterMovement)
                Player->CharacterMovement->SetMovementMode(SDK::EMovementMode::MOVE_Walking, 0);
            Player->SetActorEnableCollision(true);
            s_NoclipActive = false;
        }
    }

    // 上帝模式: 地形死亡救援 — 出图/坠落的击杀不走伤害流程 (引擎直接判 KillZ 销毁
    // 玩家, ProcessEvent 拦截不到), 只能靠位置救援: 跟踪最后一个着地点, 掉穿地图
    // (低于安全点 25m) 时拉回并清速度。穿墙飞行时不启用 (下潜会误触发)
    if (s_GodMode)
    {
        auto* Player2 = GameContext::GetPlayer();
        auto* CM = Player2 ? Player2->CharacterMovement : nullptr;
        if (Player2 && GameContext::IsUsable(Player2) && CM && !s_Noclip)
        {
            SDK::FVector Loc = Player2->K2_GetActorLocation();
            if (!CM->IsFalling())
            {
                s_GodSafeLoc = Loc;
                s_HasGodSafeLoc = true;
            }
            else if (s_HasGodSafeLoc && Loc.Z < s_GodSafeLoc.Z - 2500.0)
            {
                SDK::FVector Dest = s_GodSafeLoc;
                Dest.Z += 150.0;
                Player2->K2_SetActorLocation(Dest, false, nullptr, true);
                if (Player2->RootComponent)
                    ((SDK::UPrimitiveComponent*)Player2->RootComponent)
                        ->SetPhysicsLinearVelocity(SDK::FVector{ 0, 0, 0 }, false, SDK::FName());
                ModMenu::Log("[God] 掉穿地形救援: 拉回安全点 (%.0f, %.0f, %.0f)",
                             Dest.X, Dest.Y, Dest.Z);
            }
        }
    }

    // Gamma (UE5): r.Gamma 命令不存在 (UE4 专属), 直接改相机 PP 也实测不生效
    // (相机 PP 链被游戏覆盖/权重不明)。改 spawn 一个全局不绑定 PostProcessVolume
    // (bUnbound + 高优先级), 只覆写 ColorGamma — 引擎标准色彩分级链, 全视野生效。
    // 换世界自动重建; 关闭即销毁卷体, 不改任何游戏对象, 无需备份恢复
    static SDK::APostProcessVolume* s_GammaVolume = nullptr;
    static SDK::UWorld* s_GammaWorld = nullptr;
    {
        auto* W = GameContext::GetWorld();
        const bool VolAlive = s_GammaVolume && GameContext::IsUsable(s_GammaVolume);
        if (s_ModifyGamma && W && GameContext::IsUsable(W))
        {
            if (!VolAlive || s_GammaWorld != W)
            {
                if (VolAlive)
                    s_GammaVolume->K2_DestroyActor(); // 旧世界的卷
                s_GammaVolume = nullptr;
                if (auto* VolCls = SDK::APostProcessVolume::StaticClass())
                {
                    SDK::FTransform T{};
                    T.Rotation.X = 0; T.Rotation.Y = 0; T.Rotation.Z = 0; T.Rotation.W = 1;
                    T.Translation.X = 0; T.Translation.Y = 0; T.Translation.Z = 0;
                    T.Scale3D.X = 1; T.Scale3D.Y = 1; T.Scale3D.Z = 1;
                    auto* A = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
                        W, VolCls, T, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);
                    if (A && GameContext::IsUsable(A))
                    {
                        auto* Vol = static_cast<SDK::APostProcessVolume*>(A);
                        Vol->bUnbound = 1;      // 全局生效, 与位置无关
                        Vol->bEnabled = 1;
                        Vol->Priority = 1000.f; // 压过游戏自己的 PP 卷
                        Vol->BlendWeight = 1.f;
                        SDK::UGameplayStatics::FinishSpawningActor(A, T);
                        s_GammaVolume = Vol;
                        s_GammaWorld = W;
                        ModMenu::Log("[Visual] Gamma PP 卷已创建 (%.2f)", s_Gamma);
                    }
                }
            }
            if (s_GammaVolume && GameContext::IsUsable(s_GammaVolume))
            {
                auto& PP = s_GammaVolume->Settings;
                PP.bOverride_ColorGamma = 1;
                PP.ColorGamma.X = (double)s_Gamma;
                PP.ColorGamma.Y = (double)s_Gamma;
                PP.ColorGamma.Z = (double)s_Gamma;
                PP.ColorGamma.W = 1.0;
            }
        }
        else if (VolAlive)
        {
            s_GammaVolume->K2_DestroyActor();
            s_GammaVolume = nullptr;
            ModMenu::Log("[Visual] Gamma PP 卷已销毁, 画面恢复");
        }
    }

    // FOV: 开启 = 每帧应用滑块值; 关闭 = 恢复相机管理器的 DefaultFOV
    auto* PC = GameContext::GetPlayerController();
    if (PC && GameContext::IsUsable(PC))
    {
        if (s_ModifyFOV)
        {
            PC->FOV(s_FOV);
            s_FOVApplied = true;
        }
        else if (s_FOVApplied)
        {
            float Def = 90.f;
            if (PC->PlayerCameraManager && GameContext::IsUsable(PC->PlayerCameraManager))
                Def = PC->PlayerCameraManager->DefaultFOV;
            PC->FOV(Def);
            s_FOVApplied = false;
        }
    }

    // 修女 AI (关卡切换期半销毁对象直接跳过 — 修女缓存 1 秒才重扫, 每帧访问必须过可用性检查)
    // 两类修女无公共子类 (都直接继承 ACharacter), 字段布局不同 — 必须按类分支,
    ModMenu::g_BlockPhilippaTick = false; // 失明关闭时立即撤销 Tick 拦截
    // 盲转 Madeline 类写 Philippa = 写到错误偏移 (功能失效 + 内存踩踏)
    for (auto* Nun : GameContext::GetNuns())
    {
        if (!GameContext::IsUsable(Nun))
            continue;
        auto* Mad = Nun->IsA(SDK::ACharacter_Sister_Madelaine_Nun_C::StaticClass())
                  ? static_cast<SDK::ACharacter_Sister_Madelaine_Nun_C*>(Nun) : nullptr;
        auto* Phi = !Mad && Nun->IsA(SDK::ANun_Philippa_Character_Fat_chubby_C::StaticClass())
                  ? static_cast<SDK::ANun_Philippa_Character_Fat_chubby_C*>(Nun) : nullptr;

        // 处决链残局: 复位修女追捕状态 (dllmain 游戏线程立旗, 这里渲染线程消费)。
        // 拦下处决链后她可能卡在"抓到玩家"的分支里 — 视线停摆但听力正常。
        if (ModMenu::g_CatchNunResetPending)
        {
            if (Mad)
                Mad->IsChasingThePlayer = false;
            ModMenu::g_CatchNunResetPending = false;
            ModMenu::Log("[God] 复位修女追捕状态 (残局清理)");
        }

        // 本修女的字段备份 (首次遇到建条目; 同地址换对象时按索引差异重建)
        auto& B = NunBkFor(Nun);

        if (s_FreezeNuns)
        {
            if (!B.HasFreeze)
            {
                B.HasFreeze = true;
                if (Nun->CharacterMovement)
                    B.CMMaxWalkSpeed = Nun->CharacterMovement->MaxWalkSpeed;
                B.MaxNunSpeed = Mad ? Mad->MaxNunSpeed : (Phi ? Phi->MaxNunSpeed : 0.0);
            }
            if (Nun->CharacterMovement)
                Nun->CharacterMovement->MaxWalkSpeed = 0.f;
            if (Mad)  Mad->MaxNunSpeed = 0.0;
            if (Phi)  Phi->MaxNunSpeed = 0.0;
        }
        if (s_NunsBlind)
        {
            if (Mad)
            {
                if (!B.HasBlind)
                {
                    B.HasBlind = true;
                    B.SightMaxDistance = Mad->SightMaxDistance;
                    B.CustomSightEnabled = Mad->CustomSightEnabled;
                }
                Mad->SightMaxDistance = 0.0;
                Mad->CustomSightEnabled = true;
            }
            // Philippa 失明: 检测逻辑在 ReceiveTick 的蓝图字节码里 (改不了), 但检测结果
            // 的唯一出口是 PlayerSeen 多播委托 — 呼叫 Madeline 等监听者全绑在这上面。
            // 清空 InvocationList.Num (不释放 Data, 析构按 Max 释放, 无泄漏) = 广播时
            // 无绑定者 → "看见玩家"事件无法传播。每帧保持清空, 引擎重新绑定也无效。
            if (Phi)
            {
                // 主拦截: 吞掉 ReceiveTick (在 dllmain 的 ProcessEvent hook 里,
                // ModMenu::g_BlockPhilippaTick) — 检测逻辑全在 Tick 字节码内, 这是唯一入口
                ModMenu::g_BlockPhilippaTick = true;
                // 委托原始长度只在首次记录; 清零前检测重绑 (重绑过 = 恢复计数不安全)
                if (B.PlayerSeenNum < 0)
                {
                    B.PlayerSeenNum = DelegateNum(&Phi->PlayerSeen);
                    B.PlayerSeenRebound = false;
                }
                else if (DelegateNum(&Phi->PlayerSeen) > 0)
                    B.PlayerSeenRebound = true;
                SetDelegateNum(&Phi->PlayerSeen, 0);
                // 辅助: 清空 PlayerSeen/PhilippaScared (状态残留压制)
                Phi->PhilippaScared = false;
            }
        }
        // 失聪: Philippa 无听觉字段 (天生听不见), 只有 Madeline 需要处理
        if (s_NunsDeaf && Mad)
        {
            if (!B.HasDeaf)
            {
                B.HasDeaf = true;
                B.NunHearingDistance = Mad->NunHearingDistance;
            }
            Mad->NunHearingDistance = 0.0;
        }
        // 击杀禁用: 击杀机制 (CanKillPlayer/CanPerformPlayerKill) 为 Madeline 专属;
        // 属于上帝模式的一部分 — 否则 GodModeEnabled 挡不住修女的击杀流程
        if (s_GodMode && Mad)
        {
            if (!B.HasKill)
            {
                B.HasKill = true;
                B.CanKillPlayer = Mad->CanKillPlayer;
                B.CanPerformPlayerKill = Mad->CanPerformPlayerKill;
            }
            Mad->CanKillPlayer = false;
            Mad->CanPerformPlayerKill = false;
            // 抓捕距离封负 (2026-09-25 穿柜杀实测: 修女目击玩家进柜后 1.3s 直接
            // Catch 拉出处决 — 该链路不走 CanKillPlayer/CanPerformPlayerKill,
            // 也无可见 UFunction; 唯一抓手是 BP 每帧测距用的这两个字段。
            // 距离判定 >= CatchDistance 永不成立 → 抓捕永不触发, 感知/追踪不受影响)
            if (!B.HasCatch)
            {
                B.HasCatch = true;
                B.CatchDistance = Mad->CatchDistance;
                B.CrouchCatchDistance = Mad->CrouchCatchDistance;
                ModMenu::Log("[God] 封零修女抓捕距离 [%s] Catch=%.1f Crouch=%.1f",
                             Nun->GetName().c_str(),
                             Mad->CatchDistance, Mad->CrouchCatchDistance);
            }
            Mad->CatchDistance = -1.0;
            Mad->CrouchCatchDistance = -1.0;
        }

        // 死亡压制: 游戏的暂时死亡 (NunTemporaryDeath / Philippa.IsDead) 自带"到点自动
        // 复活"流程 (固定 30 秒)。2026-09-25 用户实测的两个问题与修法:
        //   1) 设置时长超过游戏自身复活时长 (已确认 30 秒) → 游戏先让她起身, 我们立刻
        //      补刀, 而刀落在起身演出中途, 打断复活状态机 → 她站着却被判定死亡
        //      (原地不动, 卡在生死之间)。→ 计时制压制遇到游戏自然复活直接解除 (不与
        //      游戏对抗); UI 上限同时钳到 30 秒, 超限值根本设不出来。
        //   2) 永久死亡仍要压制, 但必须等她完全起身 (无蒙太奇在播 + 1.5s 稳定期) 再补刀
        if (Mad || Phi)
        {
            for (auto& H : s_DeadHolds)
            {
                if (H.Nun != Nun)
                    continue;
                const bool Alive = Mad ? !Mad->NunTemporaryDeath : !Phi->IsDead;
                const ULONGLONG Now = GetTickCount64();
                if (!Alive)
                {
                    H.ReviveSeenTick = 0; // 还躺着
                    break;
                }
                if (H.ReviveSeenTick == 0)
                    H.ReviveSeenTick = Now;
                // 本次复活是游戏自己的计时 (不是我们到期清标志促成的) → 记一次观测值,
                // 与已知的 30 秒比对 (纯诊断, 不参与钳制)
                const bool GameOwnRevive = (H.UntilTick == ULLONG_MAX) || (Now < H.UntilTick);
                if (GameOwnRevive && H.KillTick && Now - H.KillTick > 1000)
                    NoteGameReviveMs(Now - H.KillTick, Mad ? "Madeline" : "Philippa");

                if (H.UntilTick == ULLONG_MAX)
                {
                    if (Now - H.ReviveSeenTick > 1500 && !NunPlayingMontage(Nun))
                    {
                        H.ReviveSeenTick = 0;
                        H.KillTick = Now;
                        NinaExplodeKill(Nun);
                        ModMenu::Log("[Nun] 永久死亡压制: 她已完全起身 → 再次击杀 [%s]",
                                     Nun->GetName().c_str());
                    }
                }
                else
                {
                    // 游戏先复活 = 到点, 解除压制 (清理循环随后删除本条目)
                    H.UntilTick = 0;
                    ModMenu::Log("[Nun] 游戏已自行复活 [%s] → 解除死亡压制 (不与游戏流程对抗)",
                                 Nun->GetName().c_str());
                }
                break;
            }
        }
        if (s_NunScaleEnabled)
        {
            if (!B.HasScale)
            {
                B.HasScale = true;
                B.Scale = Nun->GetActorScale3D();
            }
            Nun->SetActorScale3D(SDK::FVector{ (double)s_NunScale, (double)s_NunScale, (double)s_NunScale });
        }
    }

    // ============ 功能关闭时的字段恢复 ============
    // 主循环只在开关开启时写; 关闭后的还原统一在这里做 (每帧检查, 关闭后一帧内生效)。
    // 备份对象已销毁 (关卡切换) 的条目直接丢弃。
    if (!s_FreezeNuns || !s_NunsBlind || !s_NunsDeaf || !s_GodMode || !s_NunScaleEnabled)
    {
        for (auto It = s_NunBk.begin(); It != s_NunBk.end(); )
        {
            auto& B = It->second;
            if (!GameContext::IsUsable(B.Nun) || !ObjAliveAt(B.Nun, B.Index))
            {
                It = s_NunBk.erase(It); // 修女消失 (关卡切换), 备份随之作废
                continue;
            }
            auto* Mad = B.Nun->IsA(SDK::ACharacter_Sister_Madelaine_Nun_C::StaticClass())
                      ? static_cast<SDK::ACharacter_Sister_Madelaine_Nun_C*>(B.Nun) : nullptr;
            auto* Phi = !Mad && B.Nun->IsA(SDK::ANun_Philippa_Character_Fat_chubby_C::StaticClass())
                      ? static_cast<SDK::ANun_Philippa_Character_Fat_chubby_C*>(B.Nun) : nullptr;

            if (!s_FreezeNuns && B.HasFreeze)
            {
                if (B.Nun->CharacterMovement)
                    B.Nun->CharacterMovement->MaxWalkSpeed = B.CMMaxWalkSpeed;
                if (Mad)  Mad->MaxNunSpeed = B.MaxNunSpeed;
                if (Phi)  Phi->MaxNunSpeed = B.MaxNunSpeed;
                B.HasFreeze = false;
            }
            if (!s_NunsBlind)
            {
                if (B.HasBlind && Mad)
                {
                    Mad->SightMaxDistance = B.SightMaxDistance;
                    Mad->CustomSightEnabled = B.CustomSightEnabled;
                    B.HasBlind = false;
                }
                if (B.PlayerSeenNum >= 0 && Phi)
                {
                    // 委托计数还原 (引擎没重绑过才安全; 重绑过 = 保持现状, 引擎自管)
                    if (!B.PlayerSeenRebound && B.PlayerSeenNum > 0
                        && DelegateNum(&Phi->PlayerSeen) == 0)
                        SetDelegateNum(&Phi->PlayerSeen, B.PlayerSeenNum);
                    B.PlayerSeenNum = -1;
                }
            }
            if (!s_NunsDeaf && B.HasDeaf && Mad)
            {
                Mad->NunHearingDistance = B.NunHearingDistance;
                B.HasDeaf = false;
            }
            if (!s_GodMode && B.HasKill && Mad)
            {
                Mad->CanKillPlayer = B.CanKillPlayer;
                Mad->CanPerformPlayerKill = B.CanPerformPlayerKill;
                B.HasKill = false;
            }
            if (!s_GodMode && B.HasCatch && Mad)
            {
                Mad->CatchDistance = B.CatchDistance;
                Mad->CrouchCatchDistance = B.CrouchCatchDistance;
                B.HasCatch = false;
            }
            if (!s_NunScaleEnabled && B.HasScale)
            {
                B.Nun->SetActorScale3D(B.Scale);
                B.HasScale = false;
            }
            // 全部功能都关了且无残留备份 → 删除条目 (map 不再增长)
            if (!B.HasFreeze && !B.HasBlind && B.PlayerSeenNum < 0
                && !B.HasDeaf && !B.HasKill && !B.HasCatch && !B.HasScale)
            {
                It = s_NunBk.erase(It);
                continue;
            }
            ++It;
        }
    }

    // 死亡压制到期/失效清理: 解除压制 (只有"提前起身"模式才清死亡标志, 否则交给
    // 游戏原生复活流程 — 见 KillNun 的 ForceEarly)
    {
        ULONGLONG Now = GetTickCount64();
        for (size_t k = 0; k < s_DeadHolds.size(); )
        {
            auto* Nun = s_DeadHolds[k].Nun;
            if (!GameContext::IsUsable(Nun))
            {
                s_DeadHolds.erase(s_DeadHolds.begin() + k); // 修女消失 (关卡切换), 直接丢弃
                continue;
            }
            bool Expired = s_DeadHolds[k].UntilTick != ULLONG_MAX && Now >= s_DeadHolds[k].UntilTick;
            if (Expired)
            {
                if (s_DeadHolds[k].ForceEarly)
                {
                    if (Nun->IsA(SDK::ACharacter_Sister_Madelaine_Nun_C::StaticClass()))
                        static_cast<SDK::ACharacter_Sister_Madelaine_Nun_C*>(Nun)->NunTemporaryDeath = false;
                    else if (Nun->IsA(SDK::ANun_Philippa_Character_Fat_chubby_C::StaticClass()))
                        static_cast<SDK::ANun_Philippa_Character_Fat_chubby_C*>(Nun)->IsDead = false;
                }
                s_DeadHolds.erase(s_DeadHolds.begin() + k);
            }
            else
                k++;
        }
    }
}

// ============ 修女操作 (击杀/复活/传送, 均按单个修女粒度) ============
// 击杀走游戏原生 NinaExplode 流程 (炸药娃娃同款): 死亡动画/音效/状态全部正确
static void NinaExplodeKill(SDK::ACharacter* Nun)
{
    if (!Nun || !GameContext::IsUsable(Nun))
        return;
    if (Nun->IsA(SDK::ACharacter_Sister_Madelaine_Nun_C::StaticClass()))
        static_cast<SDK::ACharacter_Sister_Madelaine_Nun_C*>(Nun)->NinaExplode(SDK::FVector{ 0,0,0 }, 0.0);
    else if (Nun->IsA(SDK::ANun_Philippa_Character_Fat_chubby_C::StaticClass()))
        static_cast<SDK::ANun_Philippa_Character_Fat_chubby_C*>(Nun)->NinaExplode(SDK::FVector{ 0,0,0 }, 0.0);
}

static void KillNun(SDK::ACharacter* Nun)
{
    NinaExplodeKill(Nun);
    // 登记死亡压制 (时长取当前滑块; 永久死亡=永不过期)
    const ULONGLONG Now = GetTickCount64();
    const ULONGLONG Until = s_NunsStayDead ? ULLONG_MAX
        : Now + (ULONGLONG)(s_NunDeadSeconds * 1000.f);
    // 到期处理方式: 设置时长 < 游戏自身复活耗时 (30s) 才需要我们"清标志提前扶她起来";
    // 达到上限 (= 30s) 时游戏的计时会先到, 到期只解除压制、不碰她的死亡标志
    // (在游戏自己的复活流程上乱清标志同样可能把她卡在生死之间)
    const bool ForceEarly = !s_NunsStayDead && s_NunDeadSeconds + 0.5f < k_GameReviveSec;
    bool Found = false;
    for (auto& H : s_DeadHolds)
        if (H.Nun == Nun)
        {
            H.UntilTick = Until;
            H.KillTick = Now;
            H.ReviveSeenTick = 0;
            H.ForceEarly = ForceEarly;
            Found = true;
            break;
        }
    if (!Found)
        s_DeadHolds.push_back({ Nun, Until, Now, 0, ForceEarly });
}

// 击杀全部 (单个击杀的循环包装)
static void KillAllNuns()
{
    for (auto* Nun : GameContext::GetNuns())
        KillNun(Nun);
}

// 复活单个: 移除压制 + 清死亡标志
static void ReviveNun(SDK::ACharacter* Nun)
{
    for (size_t k = 0; k < s_DeadHolds.size(); k++)
        if (s_DeadHolds[k].Nun == Nun) { s_DeadHolds.erase(s_DeadHolds.begin() + k); break; }
    if (!Nun || !GameContext::IsUsable(Nun))
        return;
    if (Nun->IsA(SDK::ACharacter_Sister_Madelaine_Nun_C::StaticClass()))
        static_cast<SDK::ACharacter_Sister_Madelaine_Nun_C*>(Nun)->NunTemporaryDeath = false;
    else if (Nun->IsA(SDK::ANun_Philippa_Character_Fat_chubby_C::StaticClass()))
        static_cast<SDK::ANun_Philippa_Character_Fat_chubby_C*>(Nun)->IsDead = false;
}

// 复活全部: 清空压制列表 + 清所有修女死亡标志
static void ReviveAllNuns()
{
    s_DeadHolds.clear();
    for (auto* Nun : GameContext::GetNuns())
    {
        if (!GameContext::IsUsable(Nun))
            continue;
        if (Nun->IsA(SDK::ACharacter_Sister_Madelaine_Nun_C::StaticClass()))
            static_cast<SDK::ACharacter_Sister_Madelaine_Nun_C*>(Nun)->NunTemporaryDeath = false;
        else if (Nun->IsA(SDK::ANun_Philippa_Character_Fat_chubby_C::StaticClass()))
            static_cast<SDK::ANun_Philippa_Character_Fat_chubby_C*>(Nun)->IsDead = false;
    }
}

// 单个修女传送到玩家面前 (玩家视线前方 3 米)
static void TeleportNunToPlayer(SDK::ACharacter* Nun)
{
    auto* Player = GameContext::GetPlayer();
    if (!Nun || !Player || !GameContext::IsUsable(Nun) || !GameContext::IsUsable(Player))
        return;
    SDK::FVector PLoc = Player->K2_GetActorLocation();
    SDK::FVector Fwd = Player->GetActorForwardVector();
    SDK::FVector Dest{ PLoc.X + Fwd.X * 300.0, PLoc.Y + Fwd.Y * 300.0, PLoc.Z };
    Nun->K2_SetActorLocation(Dest, false, nullptr, true);
}

// 玩家传送到修女附近 (保持当前相对方向, 距修女 2.5 米, 避免出生在她身体里)
static void TeleportPlayerToNun(SDK::ACharacter* Nun)
{
    auto* Player = GameContext::GetPlayer();
    if (!Nun || !Player || !GameContext::IsUsable(Nun) || !GameContext::IsUsable(Player))
        return;
    SDK::FVector NLoc = Nun->K2_GetActorLocation();
    SDK::FVector PLoc = Player->K2_GetActorLocation();
    SDK::FVector Dir{ PLoc.X - NLoc.X, PLoc.Y - NLoc.Y, 0 };
    double Len = std::sqrt(Dir.X * Dir.X + Dir.Y * Dir.Y);
    if (Len < 1.0) { Dir = { 1, 0, 0 }; Len = 1.0; }
    SDK::FVector Dest{ NLoc.X + Dir.X / Len * 250.0, NLoc.Y + Dir.Y / Len * 250.0, NLoc.Z };
    Player->K2_SetActorLocation(Dest, false, nullptr, true);
}

// ============ ESP 绘制 ============
// ============ 道具位置高亮 / 场景对象选取 ============
static bool s_EspItems = false;
static char s_EspItemFilter[64] = "";       // 名称过滤 (留空 = 全部)
static bool s_EspShowGrab = true;           // 类别: 任务道具
static bool s_EspShowPhys = true;           // 类别: 物理杂物
static SDK::UObject* s_SelectedObj = nullptr;
static int32_t s_SelectedObjIndex = -1;

static std::string ToLowerStr(const char* S)
{
    std::string R = S ? S : "";
    for (auto& c : R) c = (char)tolower((unsigned char)c);
    return R;
}

// 大小写不敏感子串匹配 (Needle 需已小写)
static bool ContainsCI(const char* Hay, const std::string& Needle)
{
    if (!Hay || Needle.empty())
        return true;
    const size_t N = Needle.size();
    for (const char* p = Hay; *p; ++p)
    {
        size_t k = 0;
        while (k < N && tolower((unsigned char)p[k]) == Needle[k])
            ++k;
        if (k == N)
            return true;
    }
    return false;
}

// 选中对象存活校验 (无解引用: 只查 GObjects 槽位 — 页面已反映射的悬垂指针不可读)
static bool SelectedAlive()
{
    if (!s_SelectedObj || s_SelectedObjIndex < 0)
        return false;
    auto* Arr = SDK::UObject::GObjects.GetTypedPtr();
    return Arr && Arr->GetByIndex(s_SelectedObjIndex) == s_SelectedObj;
}

static void ClearSelection() { s_SelectedObj = nullptr; s_SelectedObjIndex = -1; }

static float Dist(const SDK::FVector& A, const SDK::FVector& B)
{
    double dx = A.X - B.X, dy = A.Y - B.Y, dz = A.Z - B.Z;
    return (float)std::sqrt(dx * dx + dy * dy + dz * dz);
}

static void DrawItemMarker(ImDrawList* dl, SDK::AActor* Obj, ImU32 Color,
                           const char* Label, SDK::FVector PlayerLoc, bool ShowDist)
{
    if (!GameContext::IsUsable(Obj))
        return;
    SDK::FVector Loc = Obj->K2_GetActorLocation();
    SDK::FVector2D S;
    if (!GameContext::WorldToScreen(Loc, S))
        return;
    // 小方框标记 + 名称
    const float W = 9.f, H = 14.f;
    dl->AddRect(ImVec2((float)S.X - W, (float)S.Y - H), ImVec2((float)S.X + W, (float)S.Y + H),
                Color, 2.f, 0, 1.2f);
    dl->AddText(ImVec2((float)S.X - W - 2.f, (float)S.Y - H - 15.f), Color, Label);
    if (ShowDist)
    {
        char DistText[48];
        snprintf(DistText, sizeof(DistText), "%.0fm", Dist(Loc, PlayerLoc) / 100.f);
        dl->AddText(ImVec2((float)S.X - W - 2.f, (float)S.Y + H + 2.f),
                    IM_COL32(255, 255, 255, 220), DistText);
    }
}

static void DrawESP()
{
    // 选中对象高亮 (独立于修女/道具开关, 选中即画; 脉冲呼吸效果)
    if (SelectedAlive())
    {
        auto* O = (SDK::AActor*)s_SelectedObj;
        SDK::FVector Loc = O->K2_GetActorLocation();
        SDK::FVector2D S;
        if (GameContext::WorldToScreen(Loc, S))
        {
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            float Pulse = 6.f + 3.f * std::sin((float)ImGui::GetTime() * 5.f);
            const float R = 14.f + Pulse;
            dl->AddCircle(ImVec2((float)S.X, (float)S.Y), R, IM_COL32(255, 255, 0, 255), 0, 2.f);
            dl->AddText(ImVec2((float)S.X - R - 2.f, (float)S.Y - R - 16.f),
                        IM_COL32(255, 255, 0, 255), L("选中", "SELECTED"));
        }
    }

    if (!s_EspNuns && !s_EspItems)
        return;

    auto* Player = GameContext::GetPlayer();
    if (!Player || !GameContext::IsUsable(Player))
        return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    SDK::FVector PlayerLoc = Player->K2_GetActorLocation();

    for (auto* Nun : GameContext::GetNuns())
    {
        if (!GameContext::IsUsable(Nun))
            continue;
        auto* Mad = Nun->IsA(SDK::ACharacter_Sister_Madelaine_Nun_C::StaticClass())
                  ? static_cast<SDK::ACharacter_Sister_Madelaine_Nun_C*>(Nun) : nullptr;
        auto* Phi = !Mad && Nun->IsA(SDK::ANun_Philippa_Character_Fat_chubby_C::StaticClass())
                  ? static_cast<SDK::ANun_Philippa_Character_Fat_chubby_C*>(Nun) : nullptr;
        SDK::FVector Loc = Nun->K2_GetActorLocation();
        SDK::FVector Head = Loc;
        Head.Z += 170.0; // 头顶位置 (估算)

        SDK::FVector2D ScreenFeet, ScreenHead;
        if (!GameContext::WorldToScreen(Loc, ScreenFeet))
            continue;
        if (!GameContext::WorldToScreen(Head, ScreenHead))
            continue;

        bool Chasing = Mad ? Mad->IsChasingThePlayer : false; // Philippa 无追击字段
        bool Dead = Mad ? Mad->NunTemporaryDeath : (Phi ? Phi->IsDead : false);
        ImU32 Color = Dead ? IM_COL32(128, 128, 128, 255)
                     : Chasing ? IM_COL32(255, 60, 60, 255) : IM_COL32(255, 120, 0, 255);

        float h = (float)(ScreenFeet.Y - ScreenHead.Y);
        float w = h * 0.45f;
        if (h < 1.f) continue;

        // 方框
        dl->AddRect(
            ImVec2((float)ScreenHead.X - w * 0.5f, (float)ScreenHead.Y),
            ImVec2((float)ScreenHead.X + w * 0.5f, (float)ScreenFeet.Y),
            Color, 0.f, 0, 1.5f);

        // 文本: 修女 + 距离 + 状态 (距离独立一行, 勾选即显示)
        ImVec2 TextPos((float)ScreenHead.X - w * 0.5f, (float)ScreenHead.Y - 18.f);
        if (TextPos.y < 2.f)
            TextPos.y = (float)ScreenHead.Y + 2.f; // 贴屏幕顶时画进方框内

        char Text[128];
        snprintf(Text, sizeof(Text), "%s%s%s", Mad ? "Madeline" : "Philippa",
                 Dead ? L(" [死亡]", " [DEAD]") : (Chasing ? L(" [追击!]", " [CHASING!]") : ""), "");
        dl->AddText(TextPos, Color, Text);

        if (s_EspDistance)
        {
            float DistVal = Dist(Loc, PlayerLoc);
            char DistText[48];
            snprintf(DistText, sizeof(DistText), "%.0fm", DistVal / 100.f);
            dl->AddText(ImVec2(TextPos.x, TextPos.y + 17.f), IM_COL32(255, 255, 255, 220), DistText);
        }
    }

    // 道具位置高亮: 按过滤条件绘制 — 文本匹配显示名/Actor 名 (大小写不敏感) +
    // 类别勾选。显示名来自实例→物品条目映射 (扫描+克隆双路径, 覆盖全部实例)
    if (s_EspItems)
    {
        GameContext::GetAllItemClasses(); // 确保扫描已跑过 (首次触发)
        // Actor 名缓存 (映射未覆盖时回退; GetName 每帧调用会抖动帧率)
        static std::unordered_map<SDK::UObject*, std::string> s_ActorNames;
        if (s_ActorNames.size() > 1024)
            s_ActorNames.clear();
        auto ActorName = [&](SDK::UObject* O) -> const char* {
            auto It = s_ActorNames.find(O);
            if (It == s_ActorNames.end())
                It = s_ActorNames.emplace(O, O->GetName()).first;
            return It->second.c_str();
        };

        const std::string Flt = ToLowerStr(s_EspItemFilter);
        auto Match = [&](SDK::UObject* O) -> const char* {
            const char* D = GameContext::GetInstanceDisplayName(O);
            if (!Flt.empty()
                && !(D && ContainsCI(D, Flt))
                && !ContainsCI(ActorName(O), Flt))
                return nullptr;
            return D ? D : ActorName(O);
        };

        if (s_EspShowGrab)
            for (auto* O : GameContext::GetGrabObjects())
                if (const char* N = Match(O))
                    DrawItemMarker(dl, O, IM_COL32(60, 230, 255, 255), N, PlayerLoc, s_EspDistance);
        if (s_EspShowPhys)
            for (auto* O : GameContext::GetPhysObjects())
                if (const char* N = Match(O))
                    DrawItemMarker(dl, O, IM_COL32(150, 160, 200, 255), N, PlayerLoc, s_EspDistance);
    }
}

// ============ 场景对象选取 ============

// 准星选物: 遍历关卡 Actor, 先用相机前向点积做粗筛 (避免 2 万次投影 ProcessEvent),
// 命中扇区内的再精确投影, 取像素距准星最近者
static void PickObjectAtCrosshair()
{
    auto* Player = GameContext::GetPlayer();
    auto* PC = GameContext::GetPlayerController();
    auto* W = SDK::UWorld::GetWorld();
    if (!Player || !GameContext::IsUsable(Player) || !PC || !W || !W->PersistentLevel)
        return;

    SDK::FVector PLoc = Player->K2_GetActorLocation();
    SDK::FVector Fwd = Player->GetActorForwardVector();
    float FwdLen = std::sqrt(Fwd.X * Fwd.X + Fwd.Y * Fwd.Y + Fwd.Z * Fwd.Z);
    if (FwdLen <= 0.f)
        return;

    SDK::AActor* Best = nullptr;
    float BestDot = 0.9938f; // cos(6.4°) — 准星附近窄扇区
    auto& Actors = W->PersistentLevel->Actors;
    for (int i = 0; i < Actors.Num(); i++)
    {
        auto* A = Actors[i];
        if (!A || !GameContext::IsUsable(A) || A == Player)
            continue;
        SDK::FVector Loc = A->K2_GetActorLocation();
        double dx = Loc.X - PLoc.X, dy = Loc.Y - PLoc.Y, dz = Loc.Z - PLoc.Z;
        double Len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (Len < 1.0)
            continue;
        float Dot = (float)((dx * Fwd.X + dy * Fwd.Y + dz * Fwd.Z) / (Len * FwdLen));
        if (Dot > BestDot)
        {
            BestDot = Dot;
            Best = A;
        }
    }
    if (!Best)
        return;
    s_SelectedObj = Best;
    s_SelectedObjIndex = Best->Index;
    ModMenu::Log("[Pick] 选中: [%s / %s] @ (%.0f, %.0f, %.0f)",
                 Best->GetName().c_str(), Best->Class ? Best->Class->GetName().c_str() : "?",
                 Best->K2_GetActorLocation().X, Best->K2_GetActorLocation().Y,
                 Best->K2_GetActorLocation().Z);
    ModMenu::g_MenuVisible = true;
}

// ============ 菜单 UI ============
// 鼠标悬浮提示: 紧跟在生成 item 的调用之后使用
static void HoverTip(const char* Desc)
{
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Desc);
}

static void TabPlayer()
{
    auto* Player = GameContext::GetPlayer();
    if (!Player)
    {
        ImGui::TextUnformatted(L("未找到玩家, 请进入游戏", "Player not found - enter the game first"));
        return;
    }

    if (ImGui::BeginTabBar("玩家子页"))
    {
        if (ImGui::BeginTabItem(L("状态", "Status")))
        {
            ImGui::Checkbox(L("上帝模式", "God Mode"), &s_GodMode);
            HoverTip(L("免疫一切伤害与击杀 (修女/摔落/蜂群/机关/爆炸), 掉出地形自动拉回; "
                       "关闭时恢复原生受伤逻辑",
                       "Immune to all damage and kills (nuns/falls/bees/traps/explosions); "
                       "auto-rescued when falling out of terrain. Restores native damage on off."));
            ImGui::Checkbox(L("无限体力", "Infinite Stamina"), &s_InfiniteStamina);
            HoverTip(L("奔跑不消耗体力; 关闭时恢复原始值",
                       "Sprinting costs no stamina; restores original value when off."));
            ImGui::Separator();
            if (ImGui::Button(L("恢复生命 (重新开始受伤逻辑)", "Restore Health (re-enable damage)")))
            {
                Player->BeingHurt = false;
            }
            HoverTip(L("清除受伤/濒死状态", "Clears hurt/dying state"));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(L("移动", "Movement")))
        {
            ImGui::Checkbox(L("启用速度修改", "Enable Speed Override"), &s_ModifySpeed);
            HoverTip(L("开启后用滑块覆盖走路/跑步/下蹲速度; 关闭时恢复原始值",
                       "Sliders override walk/run/crouch speed when on; restores originals on off."));
            ImGui::SliderFloat(L("走路速度", "Walk Speed"), &s_WalkSpeed, 50.f, 2000.f);
            ImGui::SliderFloat(L("跑步速度", "Run Speed"), &s_RunSpeed, 100.f, 3000.f);
            ImGui::SliderFloat(L("下蹲速度", "Crouch Speed"), &s_CrouchSpeed, 50.f, 1000.f);
            ImGui::Separator();
            ImGui::Checkbox(L("解锁跳跃", "Unlock Jump"), &s_ModifyJump);
            HoverTip(L("游戏未绑定跳跃, 由 mod 驱动: 按空格跳跃, 滑块调高度; 关闭时恢复",
                       "The game has no jump binding; mod-driven: Space to jump, slider sets height. Restores on off."));
            ImGui::SliderFloat(L("跳跃高度", "Jump Height"), &s_JumpHeight, 100.f, 5000.f);
            ImGui::Separator();
            ImGui::Checkbox(L("穿墙飞行 (F4)", "Noclip Fly (F4)"), &s_Noclip);
            HoverTip(L("无碰撞自由飞: WASD 平移, 空格上升, Shift 下降, F4 切换; "
                       "关闭时恢复碰撞 (高空关闭会摔落, 建议配合上帝模式)",
                       "Collision-free flight: WASD move, Space up, Shift down, F4 toggle. "
                       "Restores collision on off (falling applies - pair with God Mode)."));
            ImGui::SliderFloat(L("飞行速度", "Fly Speed"), &s_NoclipSpeed, 200.f, 5000.f, "%.0f");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(L("道具", "Items")))
        {
            if (ImGui::Button(L("获得面具", "Get Mask")))
            {
                // 调用游戏原生流程 (HUD/状态/可用性全部正确初始化), 而非裸设标志
                Player->GetBrokenMask();
                Player->CanUseMask = true;
            }
            HoverTip(L("走原生流程获得破损面具 (状态正确初始化)",
                       "Grants the broken mask via the native flow (state fully initialized)"));
            ImGui::SliderInt(L("传送点数", "Teleport Charges"), &Player->TPCharges, 0, 99);
            if (ImGui::Button(L("+10 传送点", "+10 Charges")))
                Player->TPCharges += 10;
            HoverTip(L("传送点数 +10", "Teleport charges +10"));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

static void TabNunAI()
{
    if (ImGui::BeginTabBar("修女子页"))
    {
        if (ImGui::BeginTabItem(L("控制", "Control")))
        {
            ImGui::Checkbox(L("冻结修女", "Freeze Nuns"), &s_FreezeNuns);
            HoverTip(L("所有修女定在原地; 关闭时恢复",
                       "All nuns stand still; restores on off"));
            ImGui::Checkbox(L("修女失明", "Nuns Blinded"), &s_NunsBlind);
            HoverTip(L("修女看不见你; 关闭时恢复",
                       "Nuns cannot see you; restores on off"));
            ImGui::Checkbox(L("修女失聪", "Nuns Deafened"), &s_NunsDeaf);
            HoverTip(L("Madeline 听不见声音; 关闭时恢复",
                       "Madeline cannot hear sounds; restores on off"));
            ImGui::Separator();

            // 击杀 / 死亡时间 / 复活
            // 死亡持续时间: 硬上限 = 游戏自身复活耗时 (已确认 30 秒) — 游戏到点就会
            // 自己把她扶起来, 设更长毫无意义, 只会让压制与游戏复活流程互相对抗
            if (s_NunDeadSeconds > k_GameReviveSec)
                s_NunDeadSeconds = k_GameReviveSec;
            ImGui::SliderFloat(L("死亡持续时间 (秒)", "Death Duration (s)"),
                               &s_NunDeadSeconds, 5.f, k_GameReviveSec, L("%.0f 秒", "%.0f s"));
            HoverTip(L("击杀后修女倒地时长; 上限 30 秒 = 游戏原生复活耗时, 到点游戏会自动扶起她",
                       "How long a killed nun stays down; cap 30s = the game's native revive time, after which it revives her itself"));
            if (s_GameReviveSeenMs > 0)
                ImGui::TextDisabled(L("游戏复活耗时: 30 秒 (已知) — 实测 %.1f~%.1f 秒",
                                      "Game revive time: 30s (known) - observed %.1f~%.1fs"),
                                    s_GameReviveSeenMs / 1000.0,
                                    (s_GameReviveSeenMaxMs ? s_GameReviveSeenMaxMs : s_GameReviveSeenMs) / 1000.0);
            ImGui::Checkbox(L("永久死亡", "Permanent Death"), &s_NunsStayDead);
            HoverTip(L("击杀后不再复活, 直到点\"复活所有修女\"",
                       "Killed nuns stay dead until you press \"Revive All Nuns\""));
            if (ImGui::Button(L("杀死所有修女", "Kill All Nuns")))
                KillAllNuns();
            HoverTip(L("原生死亡流程击杀全部修女, 按上方时长压制复活",
                       "Kills all nuns via the native death flow; revival suppressed per the duration above"));
            ImGui::SameLine();
            if (ImGui::Button(L("复活所有修女", "Revive All Nuns")))
                ReviveAllNuns();
            HoverTip(L("立即清除死亡压制并复活全部修女",
                       "Clears death suppression and revives all nuns immediately"));
            ImGui::Separator();

            // 传送
            if (ImGui::Button(L("所有修女传送到我面前", "Teleport All Nuns To Me")))
            {
                for (auto* Nun : GameContext::GetNuns())
                    TeleportNunToPlayer(Nun);
            }
            HoverTip(L("全部修女传到玩家面前 3 米; 单个传送在\"信息\"页",
                       "All nuns teleport 3m in front of the player; per-nun teleport on the \"Info\" tab"));
            ImGui::Separator();

            ImGui::Checkbox(L("启用体型修改", "Enable Scale Override"), &s_NunScaleEnabled);
            HoverTip(L("按倍率缩放修女体型; 关闭时恢复原始体型",
                       "Scales nuns by factor; restores original size on off"));
            ImGui::SliderFloat(L("修女体型倍率", "Nun Scale Factor"), &s_NunScale, 0.1f, 3.0f, "%.2fx");
            if (s_NunScaleEnabled && ImGui::Button(L("体型倍率归 1.0", "Reset Scale To 1.0")))
            {
                s_NunScale = 1.0f; // 直接改回倍率 (曾直接 SetActorScale3D(1.0), 下一帧就被滑块值覆写)
            }
            HoverTip(L("倍率滑块归 1.0", "Sets the scale slider back to 1.0"));
            ImGui::Separator();
            ImGui::TextUnformatted(L("提示: 修女数量由 GameContext 自动检测",
                                     "Note: nun count is auto-detected by GameContext"));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(L("信息", "Info")))
        {
            int i = 0;
            static const double s_MinD = 0.0;
            static const double s_MaxSpeed = 2000.0;
            static const double s_MaxDist = 10000.0;
            for (auto* Nun : GameContext::GetNuns())
            {
                auto* Mad = Nun->IsA(SDK::ACharacter_Sister_Madelaine_Nun_C::StaticClass())
                          ? static_cast<SDK::ACharacter_Sister_Madelaine_Nun_C*>(Nun) : nullptr;
                auto* Phi = !Mad && Nun->IsA(SDK::ANun_Philippa_Character_Fat_chubby_C::StaticClass())
                          ? static_cast<SDK::ANun_Philippa_Character_Fat_chubby_C*>(Nun) : nullptr;
                ImGui::PushID(i++);
                ImGui::Text(L("修女 #%d [%s]", "Nun #%d [%s]"), i, Mad ? "Madeline" : "Philippa");
                ImGui::SameLine();
                bool Chasing = Mad ? Mad->IsChasingThePlayer : false;
                if (Chasing)
                    ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), L("[追击中]", "[CHASING]"));
                else
                    ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), L("[平静]", "[CALM]"));
                if (Mad)
                {
                    ImGui::SliderScalar(L("最大速度", "Max Speed"), ImGuiDataType_Double, &Mad->MaxNunSpeed, &s_MinD, &s_MaxSpeed);
                    ImGui::SliderScalar(L("视野距离", "Sight Range"), ImGuiDataType_Double, &Mad->SightMaxDistance, &s_MinD, &s_MaxDist);
                    ImGui::SliderScalar(L("听觉距离", "Hearing Range"), ImGuiDataType_Double, &Mad->NunHearingDistance, &s_MinD, &s_MaxDist);
                }
                else if (Phi)
                {
                    ImGui::SliderScalar(L("最大速度", "Max Speed"), ImGuiDataType_Double, &Phi->MaxNunSpeed, &s_MinD, &s_MaxSpeed);
                    ImGui::SliderScalar(L("视野角度", "Sight Angle"), ImGuiDataType_Double, &Phi->SightAngle, &s_MinD, &s_MaxDist);
                }
                // 单个修女操作 (需要可用性检查, 缓存可能滞后)
                if (GameContext::IsUsable(Nun))
                {
                    if (ImGui::Button(L("杀死", "Kill")))
                        KillNun(Nun);
                    HoverTip(L("原生死亡流程击杀, 按控制页时长压制复活",
                               "Native death flow; revival suppressed per Control tab duration"));
                    ImGui::SameLine();
                    if (ImGui::Button(L("复活", "Revive")))
                        ReviveNun(Nun);
                    HoverTip(L("立即复活此修女", "Revive this nun immediately"));
                    ImGui::SameLine();
                    if (ImGui::Button(L("传到我面前", "Teleport To Me")))
                        TeleportNunToPlayer(Nun);
                    HoverTip(L("此修女传到玩家面前 3 米", "This nun teleports 3m in front of the player"));
                    ImGui::SameLine();
                    if (ImGui::Button(L("我传过去", "I Go To Her")))
                        TeleportPlayerToNun(Nun);
                    HoverTip(L("玩家传到此修女旁 2.5 米", "Player teleports 2.5m next to this nun"));
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

static void TabESP()
{
    ImGui::Checkbox(L("修女方框 ESP", "Nun Box ESP"), &s_EspNuns);
    HoverTip(L("绘制修女方框: 追击红色, 平静橙色; 标注种类 (Madeline/Philippa)",
               "Draws nun boxes: red when chasing, orange when calm; labels type (Madeline/Philippa)"));
    ImGui::Checkbox(L("道具位置高亮", "Item Highlight"), &s_EspItems);
    HoverTip(L("高亮道具位置: 青色=任务道具, 灰蓝=物理杂物",
               "Highlights item positions: cyan = quest items, grey-blue = physics props"));
    if (s_EspItems)
    {
        ImGui::InputTextWithHint("##espfilter", L("过滤道具名 (留空=全部)...", "Filter item names (empty = all)..."),
                                 s_EspItemFilter, sizeof(s_EspItemFilter));
        HoverTip(L("匹配显示名或内部名, 大小写不敏感; 如输入 钥匙 只显示各类钥匙",
                   "Matches display or internal names, case-insensitive; e.g. type \"Key\" to show only keys"));
        ImGui::Checkbox(L("任务道具", "Quest Items"), &s_EspShowGrab);
        ImGui::SameLine();
        ImGui::Checkbox(L("物理杂物", "Physics Props"), &s_EspShowPhys);
    }
    ImGui::Checkbox(L("显示距离", "Show Distance"), &s_EspDistance);
    HoverTip(L("方框下方显示距离 (米)", "Shows distance below boxes (meters)"));
}

static void TabVisual()
{
    ImGui::Checkbox(L("启用 FOV 修改", "Enable FOV Override"), &s_ModifyFOV);
    HoverTip(L("开启后用滑块覆盖视野角度; 关闭时恢复默认",
               "Slider overrides field of view when on; restores default on off"));
    ImGui::SliderFloat("FOV", &s_FOV, 30.f, 150.f);
    ImGui::Separator();
    ImGui::Checkbox(L("启用 Gamma 修改", "Enable Gamma Override"), &s_ModifyGamma);
    HoverTip(L("调整画面亮度 (夜视可拉高), 走相机色彩分级 gamma; 关闭恢复原始画面",
               "Adjusts brightness (raise for night vision) via camera color-grading gamma; restores on off"));
    ImGui::SliderFloat("Gamma", &s_Gamma, 0.5f, 4.f);
    ImGui::Separator();
    auto* PC = GameContext::GetPlayerController();
    if (PC && ImGui::Button(L("启用控制台作弊 (EnableCheats)", "Enable Console Cheats (EnableCheats)")))
    {
        PC->EnableCheats();
    }
    HoverTip(L("开启游戏原生作弊指令 (上帝/飞行/传送等), 之后可用游戏内控制台输入",
               "Enables native cheat commands (god/fly/teleport...), usable from the in-game console"));
}

// ============ 传送选项卡: 房间中心 / 子关卡 / 出生点 ============
static void TabTeleport()
{
    auto* Player = GameContext::GetPlayer();
    // 大小写不敏感过滤辅助
    auto ToLower = [](const std::string& s) {
        std::string r = s;
        for (auto& c : r) c = (char)tolower((unsigned char)c);
        return r;
    };

    ImGui::TextUnformatted(L("房间 / 场景 / 出生点", "Rooms / Areas / Spawn Points"));
    ImGui::Separator();
    static char s_TPFilter[64] = "";
    ImGui::InputTextWithHint("##tpfilter", L("搜索传送点...", "Search teleport points..."), s_TPFilter, sizeof(s_TPFilter));
    ImGui::SameLine();
    if (ImGui::SmallButton("x"))
        s_TPFilter[0] = '\0';
    auto& Points = GameContext::GetTeleportPoints();
    if (Points.empty())
    {
        ImGui::TextUnformatted(L("当前关卡没有可用的传送点", "No teleport points available in this level"));
    }
    else if (Player && GameContext::IsUsable(Player))
    {
        auto PLoc = Player->K2_GetActorLocation();
        const std::string Flt = ToLower(s_TPFilter);
        if (ImGui::BeginChild("##tp", ImVec2(0, 300), true))
        {
            const char* CurrentGroup = nullptr;
            int Shown = 0;
            for (auto& TP : Points)
            {
                if (Flt.empty() || ToLower(TP.Name).find(Flt) != std::string::npos)
                {
                    Shown++;
                    // 距离显示 (米)
                    double DX = TP.Pos.X - PLoc.X, DY = TP.Pos.Y - PLoc.Y, DZ = TP.Pos.Z - PLoc.Z;
                    double Dist = std::sqrt(DX * DX + DY * DY + DZ * DZ);
                    ImGui::PushID(&TP);
                    ImGui::Text("%s", TP.Name.c_str());
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 150);
                    ImGui::Text(L("%.0f 米", "%.0f m"), Dist);
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);
                    if (ImGui::SmallButton(L("传送", "Go")))
                    {
                        // 房间: 先触发子关卡加载再传送 (未加载区域是空碰撞)
                        if (TP.IsRoom && TP.SrcLevel)
                        {
                            TP.SrcLevel->SetShouldBeLoaded(true);
                            TP.SrcLevel->SetShouldBeVisible(true);
                        }
                        Player->K2_SetActorLocation(TP.Pos, false, nullptr, true);
                    }
                    ImGui::PopID();
                }
            }
            if (!Shown)
                ImGui::TextUnformatted(L("无匹配的传送点", "No matching teleport points"));
        }
        ImGui::EndChild();
    }
    HoverTip(L("房间点 = 游戏自带修女导航路点按官方分类聚合的全部场景/房间 (教堂/修女楼各层/洗衣房...), "
               "落点取路点中位 (已抬高 1.2m); 出生点 = 关卡内玩家出生位置; 进入关卡时扫描一次",
               "Room points = all areas/rooms aggregated from the game's own nun navigation waypoints "
               "(church/convent floors/laundry...), landing at the waypoint median (raised 1.2m); "
               "spawn points = player start locations; scanned once per level load"));
}

static void TabWorld()
{
    auto* Player = GameContext::GetPlayer();
    auto* GI = GameContext::GetGameInstance();
    auto* PC = GameContext::GetPlayerController();

    if (GI)
    {
        ImGui::Text(L("游戏版本: %s", "Game version: %s"), GI->GameVersion.ToString().c_str());
        ImGui::Text(L("关卡索引: %d", "Level index: %d"), GI->CurrentLevel);
        ImGui::Text(L("剩余天数: %d / %d", "Days remaining: %d / %d"), GI->DaysRemaining, GI->TotalDays);
    }
    if (Player)
    {
        auto Loc = Player->K2_GetActorLocation();
        ImGui::Text(L("玩家坐标: (%.1f, %.1f, %.1f)", "Player loc: (%.1f, %.1f, %.1f)"), Loc.X, Loc.Y, Loc.Z);
        ImGui::Text(L("安全区: %s | 洗衣房: %s | 掩体: %s", "Safe zone: %s | Laundry: %s | Bunker: %s"),
            Player->PlayerInSafeZone ? L("是", "Yes") : L("否", "No"),
            Player->PlayerInLaundry ? L("是", "Yes") : L("否", "No"),
            Player->PlayerInsideBunker ? L("是", "Yes") : L("否", "No"));
    }
    if (PC)
        ImGui::Text(L("玩家控制器: 有效", "Player controller: valid"));

    ImGui::Separator();
    if (Player && ImGui::Button(L("完成当前任务", "Complete Current Mission")))
    {
        // 触发任务完成的通用调用
        Player->CompleteMission(SDK::FString(L"AllMissions"), 0.f, true, false);
    }
}

// ============ 常见道具中文别名 (搜索用, 显示为 "原名 中文别名") ============
// 键 = 游戏内部名 (InteractText / MyselfName), 值 = 中文别名
static const std::unordered_map<std::string, const char*> s_ItemAliases = {
    // 钥匙类
    {"MainDoorKey", "大门钥匙"},   {"YellowDoorKey", "黄门钥匙"},
    {"ChurchKey", "教堂钥匙"},     {"GymKey", "体操房钥匙"},
    {"RitualKey", "仪式钥匙"},     {"LaberynthKey", "迷宫钥匙"},
    {"PinkRoomKey", "粉房钥匙"},   {"RatsKey", "老鼠钥匙"},
    {"BackdoorKey", "后门钥匙"},   {"LaundryDepotKey", "洗衣房钥匙"},
    {"ArtClassKeyM", "美术教室钥匙"}, {"LostToysKey", "玩具房钥匙"},
    {"madelineSafeKey", "Madeline保险箱钥匙"},
    // 任务道具
    {"FreshPaint", "黄漆桶"},      {"Fuse", "保险丝"},
    {"Grail", "圣杯"},             {"Dynamite", "炸药"},
    {"Doll_Normal", "普通娃娃"},   {"Doll_Explosive", "炸药娃娃"},
    {"Uncharged_Doll", "未充能娃娃"}, {"Battery", "电池"},
    {"Broom", "扫帚"},             {"Shovel", "铁锹"},
    {"Crowbar", "撬棍"},           {"Torch", "火把"},
    {"Bomb", "炸弹锅"},            {"CookedChicken", "烤鸡"},
    {"NunColaBottle", "修女可乐"}, {"Screwer", "螺丝刀"},
    {"ClockMechanism", "钟表机芯"}, {"ClockHandle", "时钟指针"},
    {"MusicalCylinder", "音乐滚筒"}, {"LeverHandle", "拉杆把手"},
    {"BrushToilet", "马桶刷"},     {"Uncogger", "通厕器"},
    {"GunGum", "泡泡糖枪"},        {"BubbleGunBody", "泡泡枪枪身"},
    {"BubbleGumContainer", "泡泡糖罐"}, {"GasCylinder", "煤气罐"},
    {"CookieBox", "饼干盒"},       {"ChickenFoodContainer", "鸡食袋"},
    // 瓶瓶罐罐
    {"MercuryFlask", "水银瓶"},    {"PeeFlask", "尿液瓶"},
    {"BloodFlask", "血液瓶"},      {"BilisFlask", "胆汁瓶"},
    {"BonesBottle", "骨头瓶"},     {"EyesBottle", "眼球瓶"},
    {"ChildFeetBottle", "小孩脚瓶"}, {"GutsBottle", "内脏瓶"},
    {"PotionContainer", "药水容器"}, {"DirtyCantimplora", "脏水壶"},
    {"FullCatPotion", "猫药水"},   {"ClothBasket", "布篮"},
    // 雕像/部件
    {"MaskPiece", "面具碎片"},     {"StatueArm", "雕像手臂"},
    {"MarlinHead", "马林鱼头"},    {"MooseHead", "驼鹿头"},
    {"SheepHead", "羊头"},         {"PigHead", "猪头"},
    {"GoatHead", "山羊头"},        {"CowHead", "牛头"},
    {"RightAngelWing", "右天使翅膀"}, {"LeftAngelWing", "左天使翅膀"},
    {"EarthStone", "土之石"},      {"MoonStone", "月之石"},
    {"SunStone", "日之石"},        {"BeeKeeperHat", "养蜂人帽"},
    {"SM_Glasses", "Madeline眼镜"}, {"figurine", "小雕像"},
    {"DressCreepy", "诡异长袍"},   {"BearToy", "玩具熊"},
    {"DefinitiveBible", "圣经"},   {"Book", "书"},
    {"GymLockerNote", "更衣室纸条"}, {"CraftRecipeCatPotion", "猫药水配方"},
    {"CraftingRecipeGumGun", "泡泡枪配方"}, {"CraftRecipeExplosiveDoll", "炸药娃娃配方"},
    {"BalloonBlueprints", "气球蓝图"},
};

static const char* GetItemAlias(const std::string& Name)
{
    auto It = s_ItemAliases.find(Name);
    return It != s_ItemAliases.end() ? It->second : nullptr;
}

static void TabItems()
{
    auto* Player = GameContext::GetPlayer();
    if (!Player)
    {
        ImGui::TextUnformatted(L("未找到玩家, 请进入游戏", "Player not found - enter the game first"));
        return;
    }

    static float s_SpawnDistance = 2.0f;
    static char s_Filter[128] = "";
    static int s_SpawnCount = 0;
    static bool s_FilterGrab = true;   // 显示任务道具
    static bool s_FilterPhys = true;   // 显示物理杂物
    ImGui::SliderFloat(L("生成距离 (米)", "Spawn Distance (m)"), &s_SpawnDistance, 0.5f, 10.f);
    ImGui::InputTextWithHint("##filter", L("搜索物品...", "Search items..."), s_Filter, sizeof(s_Filter));
    ImGui::SameLine();
    ImGui::Checkbox(L("任务道具", "Quest Items"), &s_FilterGrab);
    ImGui::SameLine();
    ImGui::Checkbox(L("物理杂物", "Physics Props"), &s_FilterPhys);
    ImGui::SameLine();
    if (ImGui::Button(L("重新扫描", "Rescan")))
        GameContext::RescanItemClasses();
    HoverTip(L("清空列表并立即重扫当前已加载的所有道具实例",
               "Clears the list and rescans all item instances currently loaded"));
    ImGui::SameLine();
    if (ImGui::Button(L("补全列表 (加载全部子关卡)", "Complete List (load all sublevels)")))
    {
        // 道具只存在于已加载的子关卡中 — 强制加载后由定期扫描自动收录
        GameContext::ForceLoadStreamingLevels();
    }
    HoverTip(L("强制加载当前地图所有流式子关卡 (远处未加载的区域), "
               "点击后等几秒, 新区域道具会由定期扫描自动加入列表; "
               "已收录条目跨关卡保留",
               "Force-loads every streaming sublevel of the current map (areas not loaded yet). "
               "Wait a few seconds - new items are picked up by the periodic scan; "
               "already-collected entries persist across levels"));
    ImGui::SameLine();
    if (ImGui::Button(L("导出实例清单", "Dump Instance List")))
        GameContext::DumpAllItemInstances();
    HoverTip(L("把所有可拾取实例的 类名/Actor名/Mesh/交互文本/Tags 全量写入日志, "
               "用于定位'列表里找不到的物品'(如漆桶)的真实身份",
               "Writes class/actor/mesh/interact-text/tags of every pickable instance to the log; "
               "use it to identify items missing from the list (e.g. the paint bucket)"));

    // 大小写不敏感过滤辅助
    auto ToLower = [](const std::string& s) {
        std::string r = s;
        for (auto& c : r) c = (char)tolower((unsigned char)c);
        return r;
    };
    std::string FilterLower = ToLower(s_Filter);

    auto& Classes = GameContext::GetAllItemClasses();
    if (Classes.empty())
    {
        ImGui::TextUnformatted(L("未找到物品 (需先进入关卡让游戏加载物品资源)",
                                 "No items found (enter a level first so the game loads item assets)"));
        return;
    }

    ImGui::Text(L("已加载 %zu 种物品 | 本次注入已生成 %d 个",
                  "Loaded %zu item types | %d spawned this session"),
                Classes.size(), s_SpawnCount);
    ImGui::Separator();

    ImGui::BeginChild("物品列表", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const char* CurrentGroup = nullptr;
    int i = 0;
    for (auto& C : Classes)
    {
        // 分组过滤 (内部键固定为中文, 与界面语言无关)
        if (Lang::IsTaskGroup(C.Group) && !s_FilterGrab)
            continue;
        if (Lang::IsPhysGroup(C.Group) && !s_FilterPhys)
            continue;

        // 搜索过滤 (大小写不敏感, 匹配显示名/Mesh 资源名/中文别名)
        if (s_Filter[0])
        {
            const char* Alias = GetItemAlias(C.Name);
            const std::string AliasLower = Alias ? ToLower(Alias) : std::string();
            if (ToLower(C.Name).find(FilterLower) == std::string::npos
                && ToLower(C.MeshName).find(FilterLower) == std::string::npos
                && AliasLower.find(FilterLower) == std::string::npos)
                continue;
        }

        // 分组标题
        if (CurrentGroup != C.Group)
        {
            CurrentGroup = C.Group;
            ImGui::SeparatorText(Lang::GroupDisplay(C.Group));
        }

        ImGui::PushID(i++);
        // 任务道具标注交互身份状态: 绿=完整交互道具; 红=已消耗 (生成时从快照恢复身份,
        // 大多数情况仍是可用道具; 快照无 Tags 才是纯外观)
        const bool IsTask = Lang::IsTaskGroup(C.Group);
        const char* Alias = GetItemAlias(C.Name);
        char Display[192];
#if MODMENU_LANG_CN
        if (Alias && Alias[0])
            snprintf(Display, sizeof(Display), "%s %s", C.Name.c_str(), Alias);
        else
#endif
            snprintf(Display, sizeof(Display), "%s", C.Name.c_str());
        if (IsTask && !GameContext::IsTemplateAlive(C))
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f),
                               L("%s (已消耗·快照恢复)", "%s (consumed - identity restored from snapshot)"), Display);
        else if (IsTask)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", Display);
        else
            ImGui::Text("%s", Display);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90);
        if (ImGui::Button(L("生成", "Spawn")))
        {
            if (GameContext::SpawnObjectFromTemplate(C, s_SpawnDistance))
                s_SpawnCount++;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ============ 对象选项卡: 视线选取场景对象 + 操作 ============
static void TabObject()
{
    auto* Player = GameContext::GetPlayer();
    if (!Player || !GameContext::IsUsable(Player))
    {
        ImGui::TextUnformatted(L("未找到玩家, 请进入游戏", "Player not found - enter the game first"));
        return;
    }

    if (ImGui::Button(L("选取视线中心目标 (F2)", "Pick Object At Crosshair (F2)")))
        PickObjectAtCrosshair();
    HoverTip(L("准星指向目标后按 F2 或点此按钮; 准星 6.4° 扇区内取最近的场景对象",
               "Aim at an object and press F2 or click this; picks the nearest actor within a 6.4 degree cone"));

    ImGui::Separator();
    if (!s_SelectedObj)
    {
        ImGui::TextDisabled(L("未选中对象", "No object selected"));
        return;
    }
    if (!SelectedAlive())
    {
        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f), L("选中对象已销毁", "Selected object was destroyed"));
        ClearSelection();
        return;
    }

    auto* O = (SDK::AActor*)s_SelectedObj;
    if (!GameContext::IsUsable(O))
    {
        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f), L("选中对象销毁中", "Selected object is being destroyed"));
        ClearSelection();
        return;
    }

    SDK::FVector Loc = O->K2_GetActorLocation();
    ImGui::Text(L("名称: %s", "Name: %s"), O->GetName().c_str());
    ImGui::Text(L("类: %s", "Class: %s"), O->Class ? O->Class->GetName().c_str() : "?");
    ImGui::Text(L("位置: (%.0f, %.0f, %.0f)  距离: %.0fm",
                  "Loc: (%.0f, %.0f, %.0f)  Dist: %.0fm"),
                Loc.X, Loc.Y, Loc.Z, Dist(Loc, Player->K2_GetActorLocation()) / 100.f);

    if (ImGui::Button(L("传送到它旁边", "Teleport To It")))
    {
        SDK::FVector Dest = Loc;
        Dest.Z += 120.0; // 胶囊半高: 防止传送进地板
        Player->K2_SetActorLocation(Dest, false, nullptr, true);
        ModMenu::Log("[Pick] 玩家传送到 [%s]", O->GetName().c_str());
    }
    HoverTip(L("玩家传送到目标位置上方 1.2m (自动落到地面)",
               "Player teleports 1.2m above the target (falls to ground)"));
    ImGui::SameLine();
    if (ImGui::Button(L("拉到我面前", "Pull To Me")))
    {
        SDK::FVector PLoc = Player->K2_GetActorLocation();
        SDK::FVector Fwd = Player->GetActorForwardVector();
        double Len = std::sqrt(Fwd.X * Fwd.X + Fwd.Y * Fwd.Y + Fwd.Z * Fwd.Z);
        if (Len > 0.0)
        {
            SDK::FVector Dest{ PLoc.X + Fwd.X / Len * 150.0,
                               PLoc.Y + Fwd.Y / Len * 150.0,
                               PLoc.Z + 30.0 };
            O->K2_SetActorLocation(Dest, false, nullptr, true);
            ModMenu::Log("[Pick] [%s] 拉到玩家面前", O->GetName().c_str());
        }
    }
    HoverTip(L("目标传送到玩家视线前方 1.5m (物理道具可能被物理引擎拉回)",
               "Target teleports 1.5m in front of the player (physics props may fall back)"));
    ImGui::SameLine();
    if (ImGui::Button(L("删除", "Delete")))
    {
        ModMenu::Log("[Pick] 删除对象 [%s / %s]", O->GetName().c_str(),
                     O->Class ? O->Class->GetName().c_str() : "?");
        O->K2_DestroyActor();
        ClearSelection();
        return;
    }
    HoverTip(L("销毁该对象 (剧情关键对象慎删, 可能影响任务流程)",
               "Destroys the object (careful with quest-critical actors)"));
    ImGui::SameLine();
    if (ImGui::Button(L("取消选中", "Clear Selection")))
        ClearSelection();
}

static void TabAbout()
{
    ImGui::TextUnformatted("Evil Nun: The Broken Mask Mod Menu");
    ImGui::TextUnformatted("ImGui + MinHook + Dumper-7 SDK");
    ImGui::Text(L("菜单语言 / Menu language: %s", "Menu language / 菜单语言: %s"), MODMENU_LANG_CODE);
    ImGui::Separator();
    ImGui::TextUnformatted(L("F1 - 切换菜单", "F1 - Toggle menu"));
    ImGui::TextUnformatted(L("ESP 在菜单关闭时依然显示", "ESP stays visible while the menu is closed"));
}

// ============ 转场冷却 (渲染线程 AV 防护, 2026-10-01 主菜单崩溃) ============
// 返回主菜单/换关卡 = GC 销毁风暴: 世界对象成批 free, 而本文件函数全部跑在
// 渲染线程, 与主线程销毁并发 — 缓存指针 (玩家/修女/PC) 可能恰在解引用瞬间被
// 释放。World 指针丢失或变更后 3 秒内冻结一切游戏对象访问 (ESP/字段写/Tab),
// 只保留热键与 ImGui 框架。GameContext::Update() 照常跑 (只读 GObjects 数组,
// 槽位校验安全), 让缓存列表尽快刷新为空。
static SDK::UWorld* s_FramePrevWorld = nullptr;
static ULONGLONG s_ObjectFreezeUntil = 0;

static bool GameObjectsFrozen()
{
    auto* W = GameContext::GetWorld();
    if (W != s_FramePrevWorld)
    {
        s_FramePrevWorld = W;
        s_ObjectFreezeUntil = GetTickCount64() + 3000;
    }
    return !W || GetTickCount64() < s_ObjectFreezeUntil;
}

void Render()
{
    const bool Frozen = GameObjectsFrozen();

    // ESP 总是绘制
    if (!Frozen)
        DrawESP();

    // 智能水印: 菜单打开时显示; 关闭时仅当鼠标悬停在左上角区域才显示 (游玩不遮挡)
    {
        const char* text = ModMenu::g_MenuVisible
            ? L("EvilNunModMenu [END 卸载]", "EvilNunModMenu [END to unload]")
            : L("EvilNunModMenu 已注入 [F1 打开菜单]", "EvilNunModMenu injected [F1 for menu]");
        ImVec2 pos(8.0f, 8.0f);
        ImVec2 size = ImGui::CalcTextSize(text);
        ImVec2 rectMax(pos.x + size.x + 8, pos.y + size.y + 4);

        bool Show = ModMenu::g_MenuVisible;
        if (!Show)
        {
            // 鼠标悬停检测 (留 6px 余量)
            ImGuiIO& io = ImGui::GetIO();
            ImVec2 m = io.MousePos;
            Show = (m.x >= pos.x - 6.f && m.x <= rectMax.x + 6.f &&
                    m.y >= pos.y - 6.f && m.y <= rectMax.y + 6.f);
        }

        if (Show)
        {
            ImU32 bg = IM_COL32(0, 0, 0, 140);
            ImU32 fg = IM_COL32(120, 220, 120, 255);
            ImGui::GetBackgroundDrawList()->AddRectFilled(pos, rectMax, bg, 4.0f);
            ImGui::GetBackgroundDrawList()->AddText(
                ImVec2(pos.x + 4, pos.y + 2), fg, text);
        }
    }

    if (!ModMenu::g_MenuVisible)
        return;

    ImGui::SetNextWindowSize(ImVec2(620, 480), ImGuiCond_FirstUseEver);
    ImGui::Begin(L("Evil Nun ModMenu [F1 切换]", "Evil Nun ModMenu [F1 toggle]"), &ModMenu::g_MenuVisible,
        ImGuiWindowFlags_NoCollapse);

    if (ImGui::BeginTabBar("#主页"))
    {
        if (Frozen)
        {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.4f, 1.f),
                L("转场中 — 对象访问已暂停 3 秒 (防销毁风暴崩溃)",
                  "Loading screen - object access paused for 3s (GC destruction storm guard)"));
        }
        else
        {
        if (ImGui::BeginTabItem(L("玩家", "Player"))) { TabPlayer(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(L("道具", "Items"))) { TabItems(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(L("修女AI", "Nun AI"))) { TabNunAI(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(L("传送", "Teleport"))) { TabTeleport(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("ESP")) { TabESP(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(L("视觉", "Visual"))) { TabVisual(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(L("对象", "Object"))) { TabObject(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(L("世界", "World"))) { TabWorld(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(L("关于", "About"))) { TabAbout(); ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void Frame()
{
    // 按 F1 切换菜单
    static bool KeyHeld = false;
    bool KeyDown = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (KeyDown && !KeyHeld)
        ModMenu::g_MenuVisible = !ModMenu::g_MenuVisible;
    KeyHeld = KeyDown;

    // 解锁跳跃: 本作输入映射没有绑跳跃动作 (EnableJump 本来就是 true, BP 跳跃事件
    // 从不触发 — 2026-08-29 诊断), 输入层无解。由 mod 直接驱动引擎跳跃: 空格按下沿
    // 调 ACharacter::Jump (引擎按 CharacterMovement.JumpZVelocity 起跳), 松开调
    // StopJumping (维持引擎按键语义, 防按住连跳)。WantCaptureKeyboard = 菜单输入框
    // 正在打字时不跳
    if (s_ModifyJump && !GameObjectsFrozen())
    {
        static bool s_SpaceHeld = false;
        bool SpaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        auto* Player = GameContext::GetPlayer();
        bool PlayerOk = Player && GameContext::IsUsable(Player) && Player->CharacterMovement;
        if (SpaceDown && !s_SpaceHeld && PlayerOk && !ImGui::GetIO().WantCaptureKeyboard)
            Player->Jump();
        else if (!SpaceDown && s_SpaceHeld && PlayerOk)
            Player->StopJumping();
        s_SpaceHeld = SpaceDown;
    }

    // F2: 选取视线中心对象 (对象页); 菜单输入框打字时不触发
    {
        static bool s_F2Held = false;
        bool F2Down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        if (F2Down && !s_F2Held && !ImGui::GetIO().WantCaptureKeyboard)
            PickObjectAtCrosshair();
        s_F2Held = F2Down;

        // ---- F4: 切换穿墙飞行 (2026-10-01) ----
        static bool s_F4Held = false;
        bool F4Down = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (F4Down && !s_F4Held && !ImGui::GetIO().WantCaptureKeyboard)
            s_Noclip = !s_Noclip;
        s_F4Held = F4Down;
    }

    GameContext::Update();
    if (!GameObjectsFrozen()) // Update 可能刚刷新 World → 重新判定 (转场即刻冻结)
        ApplyFeatures();
}

bool IsGodMode()
{
    return s_GodMode;
}

bool IsNoclip()
{
    return s_Noclip || s_NoclipActive;
}

} // namespace Menu
