#include "game_context.h"
#include "lang.h"
#include "imgui.h"

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>

namespace GameContext {

static SDK::UWorld* s_World = nullptr;
static SDK::APlayerController* s_PC = nullptr;
static SDK::AEvilNun_Player_Character_C* s_Player = nullptr;
static SDK::UEvilNunGameInstance_C* s_GI = nullptr;

static std::vector<SDK::ACharacter*> s_Nuns;
static float s_NunCacheTimer = 0.f;

static std::vector<SDK::ANunGrabObject_C*> s_GrabObjects;
static std::vector<SDK::APhysGrabObject_C*> s_PhysObjects;
static float s_ObjCacheTimer = 0.f;

// 跨帧缓存的对象引用 (指针 + GObjects 索引):
// 根因: 对象被 GC 释放后内存页可能已反映射, 任何解引用 (连读 VTable) 都会 AV;
// 唯一安全的验证方式是只查 GObjects 数组比对指针, 全程不触碰对象自身内存
struct ObjRef
{
    SDK::UObject* Ptr = nullptr;
    int32_t       Index = -1;

    static ObjRef Make(SDK::UObject* Obj) { return Obj ? ObjRef{ Obj, Obj->Index } : ObjRef{}; }

    // GObjects 对应槽位仍指向该对象 → 存活 (槽被清空/复用则已销毁)
    bool Alive() const
    {
        if (!Ptr || Index < 0)
            return false;
        auto* Arr = SDK::UObject::GObjects.GetTypedPtr();
        if (!Arr)
            return true; // 无法检查时退化为指针非空
        return Arr->GetByIndex(Index) == Ptr;
    }
};
// 修女/物品主缓存 (带索引), 对外指针向量每帧由 Alive() 重建
static std::vector<ObjRef> s_NunRefs;
static std::vector<ObjRef> s_GrabRefs;
static std::vector<ObjRef> s_PhysRefs;

static bool IsValidUObject(SDK::UObject* Obj)
{
    // 顺序关键 (2026-10-01 主菜单渲染线程 AV): 必须先查 GObjects 槽位、后读对象
    // 字段。GC 销毁风暴期 (返回主菜单/换关卡) 对象内存随时可能被 free — 槽位校验
    // 只读 GObjects 数组 (永不释放), 对象一旦从槽位移除立刻判死, 全程不碰对象内存;
    // 槽位仍指向 Obj 才说明引擎认为它有效, 此时读 VTable/Class/Flags 才安全。
    // (旧顺序先读 VTable/Class — 对象 free 后这两读直接 AV, 正是崩溃点)
    if (!Obj)
        return false;
    auto* Arr = SDK::UObject::GObjects.GetTypedPtr();
    if (!Arr)
        return Obj->VTable && Obj->Class; // 无法查表时退回基本检查 (尽力而为)
    if (Obj->Index < 0 || Obj->Index >= Arr->Num())
        return false;
    if (Arr->GetByIndex(Obj->Index) != Obj)
        return false; // 槽被清空/复用 = 已销毁
    return Obj->VTable && Obj->Class;
}

// ============ 物品模板枚举 ============
static std::vector<ItemClassInfo> s_ItemClasses;
static bool s_ItemClassesScanned = false;
static ULONGLONG s_LastItemScanTick = 0;
static std::vector<ObjRef> s_CloneActors;   // 我们生成的克隆体 (扫描时跳过, 防止顶替原版)
// 实例 → 物品条目映射 (ESP 显示名): 扫描时为每个实例记录归属条目, 克隆在生成时
// 记录; 每轮扫描整体转存上一轮再重建 — 克隆不参与模板匹配, 从上一轮带过来,
// 死对象自然淘汰 (不做无索引存活校验, 防悬垂页解引用)
static std::unordered_map<SDK::UObject*, size_t> s_InstanceToEntry;
static std::unordered_map<SDK::UObject*, size_t> s_InstanceToEntryPrev;

// 前置声明 (Update 里的定期重扫需要)
static void ScanItemsInternal();
static void ValidateDoorRedirects();

// 诊断: dump 所有存活 UWorld 实例 (结算崩溃 = 校验器发现"孤儿 World", 此快照用于定位)
// WorldType 为 native 字段 @0x112 (非 UPROPERTY, 无 SDK 偏移, 手工读取)
static void DumpAllWorlds(const char* When)
{
    auto* Arr = SDK::UObject::GObjects.GetTypedPtr();
    if (!Arr)
        return;
    const int Num = Arr->Num();
    int Count = 0;
    for (int i = 0; i < Num; i++)
    {
        SDK::UObject* O = Arr->GetByIndex(i);
        if (!O || !O->Class || O->Class != SDK::UWorld::StaticClass())
            continue;
        auto* W = static_cast<SDK::UWorld*>(O);
        const uint8_t WorldType = *(const uint8_t*)((uintptr_t)W + 0x112);
        bool Destroyed = ((uint32_t)O->Flags & ((uint32_t)SDK::EObjectFlags::BeginDestroyed
                                                | (uint32_t)SDK::EObjectFlags::FinishDestroyed)) != 0;
        ModMenu::Log("[World] %s: %p [%s] Type=%d PL=%p GI=%p%s",
                     When, (void*)W, O->GetName().c_str(), (int)WorldType,
                     (void*)W->PersistentLevel, (void*)W->OwningGameInstance,
                     Destroyed ? " (销毁中!)" : "");
        Count++;
    }
    ModMenu::Log("[World] %s: 共 %d 个 UWorld", When, Count);
}

// ============ GC 异步标志清理 (结算 Fatal 根治) ============
// Fatal 现场: "Old World not cleaned up by GC! Object NunGrabObject_C_2147478798
// is being referenced by: (PendingKill) (async) ..." — 克隆体带 (async) 内部标志。
// 机制: 生成时机撞上关卡异步加载窗口时, 引擎给新对象打 EInternalObjectFlags::AsyncLoading
// 防其加载期间被 GC; 但运行时生成的对象不属于任何加载请求, 该标志永远不会被清除
// → 对象永久逃逸 GC → 其外链 (克隆体→Level→World) 拖住整个旧 World → 结算 GC 校验 Fatal。
// 修复: 克隆体已注册进关卡 Actor 列表 (天然可达), 清除该标志后回归正常 GC 生命周期。
//
// 内部标志存储于 GObjects 数组的 FUObjectItem 中 (SDK 折叠进 Pad_8): Object@0x00, Flags(int32)@0x08
// 位值由反汇编游戏自身的标志→字符串函数确证 (EXE+0x18070A0 读 [node+0x1C] 即内部标志):
//   (Garbage)=1<<21  (native)=1<<25  (async)=1<<26  (asyncloading)=1<<27
//   (PendingKill)=1<<29  (root)=1<<30
// 注: 运行时生成的 Actor 一出生就带 Async 位 (实测两个克隆体内部标志均为 0x4000000),
// 若在其被销毁前不清除 → 销毁态+async = GC 僵尸 → 拖住旧 World → 结算 Fatal
constexpr int32_t GIF_Garbage      = 0x200000;   // 1<<21
constexpr int32_t GIF_Async        = 0x4000000;  // 1<<26
constexpr int32_t GIF_AsyncLoading = 0x8000000;  // 1<<27
constexpr int32_t GIF_PendingKill  = 0x20000000; // 1<<29

static SDK::FUObjectItem* GetObjectItem(SDK::UObject* Obj)
{
    auto* Arr = SDK::UObject::GObjects.GetTypedPtr();
    if (!Arr || !Obj || Obj->Index < 0)
        return nullptr;
    const int32_t Chunk = Obj->Index / SDK::TUObjectArray::ElementsPerChunk;
    const int32_t InChunk = Obj->Index % SDK::TUObjectArray::ElementsPerChunk;
    if (Chunk >= Arr->NumChunks || Obj->Index >= Arr->Num())
        return nullptr;
    auto** Chunks = Arr->GetDecrytedObjPtr();
    if (!Chunks || !Chunks[Chunk])
        return nullptr;
    return &Chunks[Chunk][InChunk];
}

static int32_t GetInternalFlags(SDK::UObject* Obj)
{
    auto* Item = GetObjectItem(Obj);
    return Item ? *(int32_t*)((uint8_t*)Item + 0x08) : 0;
}

static bool ClearAsyncFlags(SDK::UObject* Obj, const char* When)
{
    auto* Item = GetObjectItem(Obj);
    if (!Item || Item->Object != Obj)
        return false;
    int32_t& F = *(int32_t*)((uint8_t*)Item + 0x08);
    if (!(F & (GIF_AsyncLoading | GIF_Async)))
        return false;
    const int32_t Old = F;
    F &= ~(GIF_AsyncLoading | GIF_Async);
    if (!IsBadReadPtr(Obj, 0x20))
        ModMenu::Log("[GC] %s: 清除 [%s] async 标志 (0x%X → 0x%X)", When,
                     Obj->GetName().c_str(), Old, F);
    return true;
}

// 关卡拆除期清扫: 全 GObjects 找 "async + PendingKill" 僵尸对象。
// 正在合法异步加载的对象不会同时处于待销毁状态, 该组合必为逃逸 GC 的僵尸 —
// 正是拖住旧 World 无法回收的元凶 (含游戏侧生成的对象, 不限于我们的克隆)。
void SweepZombieAsyncFlags(const char* When)
{
    auto* Arr = SDK::UObject::GObjects.GetTypedPtr();
    if (!Arr)
        return;
    auto** Chunks = Arr->GetDecrytedObjPtr();
    if (!Chunks)
        return;
    const int32_t NumE = Arr->Num(), NumC = Arr->NumChunks;
    int Cleared = 0, ZombieLogged = 0, AsyncSeen = 0;
    for (int32_t Chunk = 0; Chunk < NumC; Chunk++)
    {
        auto* Items = Chunks[Chunk];
        if (!Items)
            continue;
        const int32_t Remain = NumE - Chunk * (int32_t)SDK::TUObjectArray::ElementsPerChunk;
        const int32_t Count = Remain < (int32_t)SDK::TUObjectArray::ElementsPerChunk
                            ? Remain : (int32_t)SDK::TUObjectArray::ElementsPerChunk;
        for (int32_t i = 0; i < Count; i++)
        {
            int32_t& F = *(int32_t*)((uint8_t*)&Items[i] + 0x08);
            if (!(F & (GIF_AsyncLoading | GIF_Async)))
                continue;
            AsyncSeen++;
            if (!(F & GIF_PendingKill))
                continue; // 加载中的合法对象不动 (只计数诊断)
            auto* Obj = Items[i].Object;
            if (Obj && !IsBadReadPtr(Obj, 0x20) && ZombieLogged < 10)
            {
                ZombieLogged++;
                ModMenu::Log("[GC] %s: 僵尸对象 [%s / %s] 内部标志 0x%X → 清除 async",
                             When, Obj->GetName().c_str(),
                             Obj->Class ? Obj->Class->GetName().c_str() : "?", F);
            }
            F &= ~(GIF_AsyncLoading | GIF_Async);
            Cleared++;
        }
    }
    // 周期清扫下 async 存活对象可能常驻 (流式 chunk 等), 只在清了僵尸时记日志防刷屏
    if (Cleared)
        ModMenu::Log("[GC] %s: async 对象 %d 个, 清除僵尸 %d 个", When, AsyncSeen, Cleared);
}

// 旧 World 标志清除: async 位本质是 GC root — 旧 World/其 PersistentLevel 自身带着
// async 位时, 即使无任何引用 GC 也不销毁它 → "not referenced... flag set" Fatal。
//
// Fatal 机理 (2026-08-28 反汇编确证): 游戏校验器(0x3C3E5A0)遍历所有存活 UWorld,
// 非 Preview 且非任何 WorldContext 当前 World 的存活 World 直接判定"未清理干净";
// 报告函数(0x3C25C60)走引用链查询, 零引用 + 仍存活 = 只可能是对象自身的
// FUObjectItem 内部标志 (async=1<<26 / asyncloading=1<<27, 位于 item+0x08,
// 与引擎 RemoveFromRoot 清 bit30 的代码同址确证) 让 GC 推迟销毁 → Fatal。
//
// IncludeCurrent 语义: LoadMap 的转场 GC 在世界指针切换【之前】执行, 旧 World 此时
// 仍是"当前"World。若跳过当前 World, 其 async 位必然带过转场 GC → 被分离后 GC 永远
// 推迟 → 正是本 Fatal (2026-08-28 实测: 全程日志无"旧 World 清扫"行即此因)。
// 清当前 World 的 async 位是安全的: 当前 World 处于引擎根集保护下, GC 绝不会因
// 清位而误收; 该位只在它被分离后才成为致命引信。
// IncludeCurrent=false 时仍保护当前 World (保守模式, 供特殊场景调用)。
void ClearOldWorldAsyncFlags(bool IncludeCurrent)
{
    auto* Arr = SDK::UObject::GObjects.GetTypedPtr();
    if (!Arr)
        return;
    auto** Chunks = Arr->GetDecrytedObjPtr();
    if (!Chunks)
        return;
    auto* Cur = SDK::UWorld::GetWorld();
    const int32_t NumE = Arr->Num(), NumC = Arr->NumChunks;
    for (int32_t Chunk = 0; Chunk < NumC; Chunk++)
    {
        auto* Items = Chunks[Chunk];
        if (!Items)
            continue;
        const int32_t Remain = NumE - Chunk * (int32_t)SDK::TUObjectArray::ElementsPerChunk;
        const int32_t Count = Remain < (int32_t)SDK::TUObjectArray::ElementsPerChunk
                            ? Remain : (int32_t)SDK::TUObjectArray::ElementsPerChunk;
        for (int32_t i = 0; i < Count; i++)
        {
            auto* Obj = Items[i].Object;
            if (!Obj || !Obj->Class || IsBadReadPtr(Obj, 0x20))
                continue;
            if (Obj->Class != SDK::UWorld::StaticClass() && Obj->Class != SDK::ULevel::StaticClass())
                continue;
            int32_t& F = *(int32_t*)((uint8_t*)&Items[i] + 0x08);
            if (!(F & (GIF_AsyncLoading | GIF_Async)))
                continue;
            // 当前活动 World 及其 PersistentLevel: 仅在调用方明确要求时一并清
            // (当前 World 受根集保护, 清位无风险; 旧 World 转场前必须清干净)
            bool IsCur = Cur && (Obj == Cur || Obj == Cur->PersistentLevel);
            if (IsCur && !IncludeCurrent)
                continue;
            ModMenu::Log("[GC] 旧 World 清扫: [%s]%s 内部 0x%X UObjectFlags=0x%llX → 清除 async",
                         Obj->GetName().c_str(), IsCur ? " (旧=当前)" : "",
                         F, (unsigned long long)Obj->Flags);
            F &= ~(GIF_AsyncLoading | GIF_Async);
        }
    }
}

// 我们生成的克隆体清扫 (本体 + 组件): 组件与本体的 async 标志同样会拖住
// Actor → Level → World 整条链, 必须一并清除
static void ClearCloneAsyncFlags(const char* When)
{
    for (auto& R : s_CloneActors)
    {
        if (!R.Ptr || !R.Alive())
            continue;
        ClearAsyncFlags(R.Ptr, When);
        // 组件枚举走 ProcessEvent, 需对象未处于拆除流程
        if (IsUsable(R.Ptr))
        {
            auto Comps = ((SDK::AActor*)R.Ptr)->K2_GetComponentsByClass(
                SDK::UActorComponent::StaticClass());
            for (int i = 0; i < Comps.Num(); i++)
                if (Comps[i])
                    ClearAsyncFlags(Comps[i], When);
        }
    }
}

// 玩家丢失时刻捕获的旧 World 指针 (演出期清扫的锚点: LoadMap 后 s_World 变成新
// World, 绝不能对新 World 正在合法异步加载的对象清标志; 只做指针比较, 不解引用)
static SDK::UWorld* s_OldWorld = nullptr;

// 锚定 World 的对象 async 全量清扫 — "not cleaned up by GC" Fatal 的根治之一。
// Fatal 链: 运行期生成的 Actor 出生带 async 位 → 销毁后成 PendingKill+async 僵尸
// → 其 Outer 链 (Actor→Level→World) 拖住旧 World → LoadMap GC 校验 Fatal。
// 只清"已销毁"僵尸有致命盲区: 对象销毁到 GC 之间可能小于清扫间隔 (实测 1 秒内,
// 上轮 Fatal 的 FloatingInteractActor 在最后一次清扫时还活着)。
// 本清扫把 Outer 链指向锚点 World 的【所有】对象 (含存活) 的 async 位清掉:
//   - 存活对象经 Level Actor 列表保持 GC 可达, 清位不会被误收;
//   - 之后无论何时被销毁都不再成僵尸 — 在销毁发生之前拆掉引信, 不依赖销毁后时机。
// 周期清扫用当前活跃 World 作锚 (根集保护下, 清位零风险 — 引信在转场发生前拆除);
// 玩家丢失后用 s_OldWorld 作锚 (LoadMap 后 s_World 已是新 World, 绝不能碰新 World
// 正在异步加载的对象 — 新 World 的加载中对象 Outer 链不经过旧 World)。
static void SweepWorldObjectsAsync(SDK::UWorld* Anchor, const char* When)
{
    if (!Anchor)
        return;
    auto* Arr = SDK::UObject::GObjects.GetTypedPtr();
    if (!Arr)
        return;
    auto** Chunks = Arr->GetDecrytedObjPtr();
    if (!Chunks)
        return;
    const int32_t NumE = Arr->Num(), NumC = Arr->NumChunks;
    int Cleared = 0;
    for (int32_t Chunk = 0; Chunk < NumC; Chunk++)
    {
        auto* Items = Chunks[Chunk];
        if (!Items)
            continue;
        const int32_t Remain = NumE - Chunk * (int32_t)SDK::TUObjectArray::ElementsPerChunk;
        const int32_t Count = Remain < (int32_t)SDK::TUObjectArray::ElementsPerChunk
                            ? Remain : (int32_t)SDK::TUObjectArray::ElementsPerChunk;
        for (int32_t i = 0; i < Count; i++)
        {
            int32_t& F = *(int32_t*)((uint8_t*)&Items[i] + 0x08);
            if (!(F & (GIF_AsyncLoading | GIF_Async)))
                continue;
            // Outer 链上行 (≤16 层) 是否到达锚点 World (纯指针比较)
            SDK::UObject* O = Items[i].Object;
            bool InAnchor = false;
            for (int d = 0; O && d < 16; d++)
            {
                if (O == (SDK::UObject*)Anchor) { InAnchor = true; break; }
                O = O->Outer;
            }
            if (!InAnchor)
                continue;
            F &= ~(GIF_AsyncLoading | GIF_Async);
            Cleared++;
        }
    }
    if (Cleared > 0)
        ModMenu::Log("[GC] %s: 锚定 World 对象清除 async 位 %d 个", When, Cleared);
}

// 玩家丢失时刻捕获的旧 World 指针 (演出期清扫的锚点) — 见 SweepWorldObjectsAsync
static void SweepOldWorldAsync(const char* When)
{
    SweepWorldObjectsAsync(s_OldWorld, When);
}

// [2026-08-29 移除玩家丢失时的 DestroyAllClones]
// 曾经在此主动 K2_DestroyActor 销毁所有克隆体 (GC Fatal 的早期根治手段):
// 当时克隆体的 async 标志只能靠生成时/僵尸清扫两条窄路径清除, 主动销毁让
// 未消耗克隆走正常拆除流程。现在周期清扫 (500ms) 已常态化清干净全部克隆的
// async 位, 主动销毁不再有收益, 反而有真实危害:
//   玩家丢失 ≠ 换关卡 — 死亡 (被车撞等) 也触发, 此刻死亡流程可能仍引用玩家
//   手持的克隆 (NunGrabbedRightHandActor), 中途 Destroy 是 UAF 窗口 —
//   2026-08-29 返回主菜单的 AV (EXE+0x2A060BF, 容器容量/缓冲不匹配的越界写)
//   正发生在死亡后紧随克隆销毁之时。
// 克隆随重生保留还带来更好体验: 身份持有者继续有效, 已消耗判定不变。
// 换关卡时克隆由引擎随关卡原生拆除 (async 已清 → 无僵尸风险)。

void Update()
{
    // 关卡切换检测 (诊断: 定位崩溃与关卡切换的时序关系)
    static SDK::UWorld* s_LastWorld = nullptr;
    s_World = SDK::UWorld::GetWorld();

    // 僵尸对象常态清扫 (每 500ms) — 必须放在所有提前 return 之前:
    // LoadMap 过渡期 GetWorld() 返回无效, 下方的玩家丢失检测/周期清扫全都到不了
    // (上轮 Fatal: 返回主菜单时 BodySetup 漏网, 日志全程无"玩家丢失"即此因)。
    // GC 校验正好发生在过渡窗口, 只有此处能覆盖。判定 async+PendingKill 双条件,
    // 正常异步加载中的对象 (无 PendingKill) 不会被误伤
    static ULONGLONG s_LastZombieSweep = 0;
    if (GetTickCount64() - s_LastZombieSweep > 500)
    {
        s_LastZombieSweep = GetTickCount64();
        SweepZombieAsyncFlags("周期");
        // 活跃 World 引信常态拆除 (2026-08-28 "not cleaned up by GC" Fatal 根治):
        // 反汇编确证校验器要求"非当前/非 Preview 的存活 World 必须已被 GC", 而旧
        // World 的 async 位在转场 GC 时仍属"当前 World" — 校验器入口清扫来不及
        // (GC 已先行执行)。根治 = 在正常游玩态 (当前 World 受根集保护, 清位零风险)
        // 常态清干净: 之后无论何时转场, 旧 World 的 async 位都是干净的, 与清扫和
        // 转场 GC 之间的时机完全无关。
        // 仅在 World 有效时执行: 过渡窗口 GetWorld() 可能为空, 此时新 World 可能
        // 正在异步加载 (其 async 位是合法保护), 绝不能碰。
        if (s_World && IsValidUObject(s_World))
        {
            ClearOldWorldAsyncFlags(true);
            SweepWorldObjectsAsync(s_World, "周期");
        }
    }
    if (s_World != s_LastWorld)
    {
        ModMenu::Log("[GameContext] World 切换: %p -> %p", (void*)s_LastWorld, (void*)s_World);
        s_LastWorld = s_World;
        s_OldWorld = nullptr;   // 锚点作废: 新 World 的加载中对象绝不可清标志
        DumpAllWorlds("切换时");
        // 旧 World 的 GC 校验可能已在本帧前后执行, 清扫残余僵尸 (兜底)
        ClearCloneAsyncFlags("切换时");
        SweepZombieAsyncFlags("切换时");
    }
    if (!IsValidUObject(s_World))
    {
        s_World = nullptr; s_PC = nullptr; s_Player = nullptr; s_GI = nullptr;
        s_NunRefs.clear(); s_GrabRefs.clear(); s_PhysRefs.clear();
        s_Nuns.clear(); s_GrabObjects.clear(); s_PhysObjects.clear();
        return;
    }

    auto* GI = s_World->OwningGameInstance;
    if (!IsValidUObject(GI))
    {
        s_PC = nullptr; s_Player = nullptr; s_GI = nullptr;
        return;
    }

    // LocalPlayers[0]->PlayerController->Character
    s_PC = nullptr;
    s_Player = nullptr;
    auto* LocalPlayers = &GI->LocalPlayers;
    if (LocalPlayers->Num() > 0)
    {
        auto* LP = (*LocalPlayers)[0];
        if (IsValidUObject(LP))
        {
            auto* PC = static_cast<SDK::APlayerController*>(LP->PlayerController);
            if (IsValidUObject(PC))
            {
                s_PC = PC;
                auto* Ch = PC->Character;
                if (IsValidUObject(Ch) && Ch->IsA(SDK::AEvilNun_Player_Character_C::StaticClass()))
                    s_Player = static_cast<SDK::AEvilNun_Player_Character_C*>(Ch);
            }
        }
    }

    s_GI = static_cast<SDK::UEvilNunGameInstance_C*>(GI);

    // 状态变化诊断日志 (仅在变化时输出一次)
    static bool s_HadPlayer = false;
    bool HasPlayer = (s_Player != nullptr);
    if (HasPlayer != s_HadPlayer)
    {
        s_HadPlayer = HasPlayer;
        ModMenu::Log("[GameContext] 玩家 %s (World=%p PC=%p)",
                     HasPlayer ? "已找到" : "丢失", (void*)s_World, (void*)s_PC);
        // 玩家丢失 = 关卡结束/结算时刻 (正是历史崩溃点), 留下 World 快照
        if (!HasPlayer)
        {
            DumpAllWorlds("玩家丢失");
            // 演出期清扫锚点: 此刻的 World 即将被替换, 记录供后续按 Outer 链定位
            s_OldWorld = s_World;
            // 旧 World 全量 async 清扫 (销毁前拆引信) + 克隆体标志清扫 + 僵尸清扫
            // (不销毁克隆 — 见上方 DestroyAllClones 移除说明)
            SweepOldWorldAsync("玩家丢失");
            ClearCloneAsyncFlags("玩家丢失");
            SweepZombieAsyncFlags("玩家丢失");
        }
    }

    // 结局演出期 (玩家已丢失, 旧 World 尚未替换): 游戏生成的 Actor (如结局的
    // FloatingInteractActor) 一出生带 Async 位 — 周期全量清扫旧 World 对象,
    // 在其被销毁【前】拆除引信 (250ms 间隔, 远小于结局演出时长)。
    // 锚点用 s_OldWorld 而非 s_World: LoadMap 后 s_World 已是新 World,
    // 此时清的必须是旧 World 残余, 绝不能碰新 World 正在加载的对象。
    static ULONGLONG s_LastCinematicSweep = 0;
    if (!HasPlayer && s_OldWorld
        && GetTickCount64() - s_LastCinematicSweep > 250)
    {
        s_LastCinematicSweep = GetTickCount64();
        SweepOldWorldAsync("演出期");
        SweepZombieAsyncFlags("演出期");
    }

    // 关卡切换/加载期 (玩家无效): 游戏线程正并发批量创建/销毁对象,
    // 渲染线程此时遍历关卡 Actor 数组 / GObjects 会与游戏线程竞争 → 崩溃;
    // 玩家存在 = 处于稳定游玩态, 才允许扫描
    const bool GameplayStable = s_Player && IsUsable(s_Player);

    // 修女缓存, 每 1 秒重新搜索一次
    s_NunCacheTimer -= ImGui::GetIO().DeltaTime;
    if (s_NunCacheTimer <= 0.f)
    {
        s_NunCacheTimer = 1.f;
        if (!GameplayStable)
        {
            // 切换期: 只清空缓存 (引用半销毁对象同样危险), 不做任何遍历
            s_NunRefs.clear();
            s_GrabRefs.clear();
            s_PhysRefs.clear();
            s_Nuns.clear();
            s_GrabObjects.clear();
            s_PhysObjects.clear();
            return;
        }
        s_NunRefs.clear();
        s_GrabRefs.clear();
        s_PhysRefs.clear();
        if (s_World && s_World->PersistentLevel)
        {
            auto& Actors = s_World->PersistentLevel->Actors;
            for (int i = 0; i < Actors.Num(); i++)
            {
                auto* Actor = Actors[i];
                if (!IsUsable(Actor)) // 半销毁对象不进缓存 (关卡切换期)
                    continue;
                if (Actor->IsA(SDK::ACharacter_Sister_Madelaine_Nun_C::StaticClass())
                    || Actor->IsA(SDK::ANun_Philippa_Character_Fat_chubby_C::StaticClass()))
                {
                    s_NunRefs.push_back(ObjRef::Make(Actor));
                }
                else if (Actor->IsA(SDK::ANunGrabObject_C::StaticClass()))
                {
                    s_GrabRefs.push_back(ObjRef::Make(Actor));
                }
                else if (Actor->IsA(SDK::APhysGrabObject_C::StaticClass()))
                {
                    s_PhysRefs.push_back(ObjRef::Make(Actor));
                }
            }

            // 关卡扫描结果诊断 (数量变化时输出)
            static size_t s_LastNunCount = (size_t)-1;
            static size_t s_LastObjCount = (size_t)-1;
            size_t nunCount = s_NunRefs.size();
            size_t objCount = s_GrabRefs.size() + s_PhysRefs.size();
            if (nunCount != s_LastNunCount || objCount != s_LastObjCount)
            {
                s_LastNunCount = nunCount;
                s_LastObjCount = objCount;
                ModMenu::Log("[GameContext] 扫描: 修女 %zu 个, 物品 %zu 个 (关卡 Actor 总数 %d)",
                             nunCount, objCount, Actors.Num());
            }
        }
    }

    // 每帧: 无解引用校验重建对外指针缓存 (修女缓存里的对象可能已被 GC 释放,
    // 页面可能已反映射 — 绝不能用会解引用对象的 IsValidUObject, 曾导致关卡切换崩溃)
    s_NunRefs.erase(std::remove_if(s_NunRefs.begin(), s_NunRefs.end(),
        [](const ObjRef& R) { return !R.Alive(); }), s_NunRefs.end());
    s_GrabRefs.erase(std::remove_if(s_GrabRefs.begin(), s_GrabRefs.end(),
        [](const ObjRef& R) { return !R.Alive(); }), s_GrabRefs.end());
    s_PhysRefs.erase(std::remove_if(s_PhysRefs.begin(), s_PhysRefs.end(),
        [](const ObjRef& R) { return !R.Alive(); }), s_PhysRefs.end());
    s_Nuns.clear();
    for (auto& R : s_NunRefs)   s_Nuns.push_back((SDK::ACharacter*)R.Ptr);
    s_GrabObjects.clear();
    for (auto& R : s_GrabRefs)  s_GrabObjects.push_back((SDK::ANunGrabObject_C*)R.Ptr);
    s_PhysObjects.clear();
    for (auto& R : s_PhysRefs)  s_PhysObjects.push_back((SDK::APhysGrabObject_C*)R.Ptr);

    // 物品模板定期增量重扫 (每 10 秒, 合并不清除 — 随玩家游玩积累全游戏物品)
    // 关卡切换期禁止: GObjects 正被游戏线程并发增删, 遍历会竞争崩溃
    if (GameplayStable && s_ItemClassesScanned && GetTickCount64() - s_LastItemScanTick > 10000)
        ScanItemsInternal();
}

SDK::UWorld* GetWorld() { return s_World; }

bool IsUsable(SDK::UObject* Obj)
{
    if (!IsValidUObject(Obj))
        return false;
    // 引擎已标记销毁的对象 (GC 阶段) 不可访问
    constexpr uint32_t Dead = (uint32_t)SDK::EObjectFlags::BeginDestroyed
                            | (uint32_t)SDK::EObjectFlags::FinishDestroyed;
    if ((uint32_t)Obj->Flags & Dead)
        return false;
    // Actor: Destroy() 调用瞬间即置位 bActorIsBeingDestroyed (不等 GC),
    // 此时组件已开始拆除 — 比 UObject flags 更早、更可靠的生命周期信号
    if (Obj->IsA(SDK::AActor::StaticClass()))
        return !((SDK::AActor*)Obj)->bActorIsBeingDestroyed;
    return true;
}
SDK::APlayerController* GetPlayerController() { return s_PC; }
SDK::AEvilNun_Player_Character_C* GetPlayer() { return s_Player; }
SDK::UEvilNunGameInstance_C* GetGameInstance() { return s_GI; }
const std::vector<SDK::ACharacter*>& GetNuns() { return s_Nuns; }
const std::vector<SDK::ANunGrabObject_C*>& GetGrabObjects() { return s_GrabObjects; }
const std::vector<SDK::APhysGrabObject_C*>& GetPhysObjects() { return s_PhysObjects; }

// ============ 物品模板枚举 ============

// FString -> std::string 并去首尾空白
static std::string FStringToStr(const SDK::FString& S)
{
    if (!S.IsValid())
        return "";
    std::string R = S.ToString();
    while (!R.empty() && (unsigned char)R.front() <= ' ')
        R.erase(R.begin());
    while (!R.empty() && (unsigned char)R.back() <= ' ')
        R.pop_back();
    return R;
}

// ============ 实例级属性通用捕获/回放 ============
// 手挑字段清单覆盖不了每件道具的实例级配置: 如小雕像 (figurine) 的 BreakPieces
// 碎片网格 (TArray<UStaticMesh*>) 是关卡里逐实例配置的, CDO 默认为空 → 克隆体
// 摔不碎。反射遍历类属性链捕获两类安全数据:
//   1) 原始类型 (Bool/Byte/Enum/Int/Int64/Float/Double, 非 Transient) → 逐字节快照
//   2) 对象数组 (TArray<UObject*>) → 指针+GObjects 索引成对捕获 (如 BreakPieces)
// 刻意不碰: 对象单指针 (语义不明)、字符串/FText (克隆体自己的构造分配必须保留 —
// memcpy 会共享缓冲, 销毁时 double-free)、委托 (复制绑定会让克隆收到原版的事件)、
// 结构体 (Configuration 已按字段安全拷贝)、FName/名字数组 (Tags 已单独处理)。
// 类型判定按 FFieldClass 的名字 FName 比较 — CastFlags 在该引擎上不稳定 (见
// CopyMulticastDelegates 的 A/B/C 轮日志结论), FName 解析一次后按索引比较零开销。

// 引擎分配器通道的数组深拷贝 (定义在"身份字段移交"区, 此处前向声明)
static void DeepCopyArrayProp(SDK::UObject* Dest, SDK::FName PropName, const void* SrcArray);

// 属性种类
enum class PropKind { Other, Bool, Prim, ObjectArray };

static PropKind ClassifyProperty(SDK::FField* F)
{
    static int32_t NameBool = 0, NameByte = 0, NameEnum = 0, NameInt = 0,
                   NameInt64 = 0, NameFloat = 0, NameDouble = 0,
                   NameArray = 0, NameObj = 0, NameClass = 0;
    static bool Init = false;
    if (!Init)
    {
        Init = true;
        NameBool   = SDK::BasicFilesImplUtils::StringToName(L"BoolProperty").ComparisonIndex;
        NameByte   = SDK::BasicFilesImplUtils::StringToName(L"ByteProperty").ComparisonIndex;
        NameEnum   = SDK::BasicFilesImplUtils::StringToName(L"EnumProperty").ComparisonIndex;
        NameInt    = SDK::BasicFilesImplUtils::StringToName(L"IntProperty").ComparisonIndex;
        NameInt64  = SDK::BasicFilesImplUtils::StringToName(L"Int64Property").ComparisonIndex;
        NameFloat  = SDK::BasicFilesImplUtils::StringToName(L"FloatProperty").ComparisonIndex;
        NameDouble = SDK::BasicFilesImplUtils::StringToName(L"DoubleProperty").ComparisonIndex;
        NameArray  = SDK::BasicFilesImplUtils::StringToName(L"ArrayProperty").ComparisonIndex;
        NameObj    = SDK::BasicFilesImplUtils::StringToName(L"ObjectProperty").ComparisonIndex;
        NameClass  = SDK::BasicFilesImplUtils::StringToName(L"ClassProperty").ComparisonIndex;
    }
    if (!F->ClassPrivate)
        return PropKind::Other;
    const int32_t CN = F->ClassPrivate->Name.ComparisonIndex;
    if (CN == NameBool)   return PropKind::Bool;
    if (CN == NameByte || CN == NameEnum || CN == NameInt || CN == NameInt64
        || CN == NameFloat || CN == NameDouble)
        return PropKind::Prim;
    if (CN == NameArray)
    {
        auto* Arr = (SDK::FArrayProperty*)F;
        if (Arr->InnerProperty && Arr->InnerProperty->ClassPrivate)
        {
            const int32_t InnerName = Arr->InnerProperty->ClassPrivate->Name.ComparisonIndex;
            if (InnerName == NameObj || InnerName == NameClass)
                return PropKind::ObjectArray;
        }
    }
    return PropKind::Other;
}

// 捕获单个实例的全部实例级属性 (走类继承链)
static void CaptureInstanceProps(SDK::UObject* Src, ItemConfigSnapshot& Out)
{
    if (!Src || !Src->Class || IsBadReadPtr(Src, 0x20))
        return;
    // CPF_Transient: 运行态字段 (计数器/临时状态) 不捕获 — 只取设计态
    constexpr uint64_t CPF_Transient = 0x10000ull;
    for (SDK::UStruct* S = Src->Class; S; S = S->SuperStruct)
    {
        if (S == SDK::UObject::StaticClass())
            break; // UObject 基类无设计态
        for (SDK::FField* F = S->ChildProperties; F; F = F->Next)
        {
            auto* P = (SDK::FProperty*)F;
            if (!P || (P->PropertyFlags & CPF_Transient) || P->Offset < 0)
                continue;
            switch (ClassifyProperty(F))
            {
            case PropKind::Bool:
            {
                // 位域安全: 只取本位的掩码值 (同 32 位字里的其它位可能是引擎标志)
                auto* B = (SDK::FBoolProperty*)F;
                if (B->ByteMask == 0 || B->ByteOffset > 0xFF00)
                    break;
                ItemConfigSnapshot::PrimSnap PS{};
                PS.Offset = (uint16_t)B->ByteOffset;
                PS.Size = 1;
                PS.Mask = B->ByteMask;
                PS.Data[0] = (*((const uint8_t*)Src + B->ByteOffset) & B->ByteMask) ? B->ByteMask : 0;
                Out.Prims.push_back(PS);
                break;
            }
            case PropKind::Prim:
            {
                const int32_t Size = P->ElementSize;
                if (Size <= 0 || Size > 8)
                    break;
                ItemConfigSnapshot::PrimSnap PS{};
                PS.Offset = (uint16_t)P->Offset;
                PS.Size = (uint8_t)Size;
                PS.Mask = 0xFF;
                memcpy(PS.Data, (const uint8_t*)Src + P->Offset, Size);
                Out.Prims.push_back(PS);
                break;
            }
            case PropKind::ObjectArray:
            {
                auto& Arr = *(SDK::TArray<SDK::UObject*>*)((uint8_t*)Src + P->Offset);
                if (Arr.Num() <= 0 || Arr.Num() > 64)
                    break;
                ItemConfigSnapshot::ObjArraySnap OA;
                OA.PropName = F->Name.ToString();
                for (int32_t k = 0; k < Arr.Num(); k++)
                {
                    SDK::UObject* El = Arr[k];
                    if (El)
                    {
                        OA.Ptrs.push_back(El);
                        OA.Indexes.push_back(El->Index);
                    }
                }
                if (!OA.Ptrs.empty())
                    Out.ObjArrays.push_back(std::move(OA));
                break;
            }
            default:
                break;
            }
        }
    }
}

// 回放实例级属性到克隆体 (CloneItemActor 在 FinishSpawning 之后调用, 防构造脚本覆盖)
static void ApplyInstanceProps(SDK::AActor* Dest, const ItemConfigSnapshot& S)
{
    if (!Dest || IsBadReadPtr(Dest, 0x20))
        return;
    for (const auto& PS : S.Prims)
    {
        if (PS.Size == 0 || PS.Size > 8)
            continue;
        uint8_t* Dst = (uint8_t*)Dest + PS.Offset;
        if (PS.Mask == 0xFF)
            memcpy(Dst, PS.Data, PS.Size);
        else
            *Dst = (uint8_t)((*Dst & ~PS.Mask) | (PS.Data[0] & PS.Mask));
    }
    for (const auto& OA : S.ObjArrays)
    {
        // 元素存活校验 (跨关卡后碎片网格等资产可能已卸载, 悬垂指针页面不可读)
        std::vector<SDK::UObject*> Alive;
        for (size_t i = 0; i < OA.Ptrs.size(); i++)
        {
            int32_t Idx = i < OA.Indexes.size() ? OA.Indexes[i] : -1;
            if (OA.Ptrs[i] && ObjRef{ OA.Ptrs[i], Idx }.Alive())
                Alive.push_back(OA.Ptrs[i]);
        }
        if (Alive.empty())
            continue;
        SDK::TArray<SDK::UObject*> Src(Alive.data(), (int32_t)Alive.size(), (int32_t)Alive.size());
        std::wstring Wide(OA.PropName.begin(), OA.PropName.end());
        SDK::FName PropFName = SDK::BasicFilesImplUtils::StringToName(Wide.c_str());
        DeepCopyArrayProp(Dest, PropFName, &Src);
        ModMenu::Log("[Spawn]   实例属性回放: %s (%d 项)", OA.PropName.c_str(), (int)Alive.size());
    }
}

// 从存活实例捕获类型化配置快照 (SDK 成员直接访问, 偏移由 Dumper-7 断言保证)
static void CaptureItemConfig(SDK::AActor* Actor, bool IsGrab, ItemConfigSnapshot& Out)
{
    Out = ItemConfigSnapshot{};
    Out.TemplateScale = Actor->GetActorScale3D();
    // Actor 名 + Tags: 场景交互 (保险丝盒等) 按名/标签识别道具
    Out.ActorName = Actor->Name;
    for (int32_t k = 0; k < Actor->Tags.Num(); k++)
        Out.Tags.push_back(Actor->Tags[k]);

    if (IsGrab)
    {
        auto* G = static_cast<SDK::ANunGrabObject_C*>(Actor);
        // 含 FString 的结构用 memcpy (裸字节共享, 避免容器拷贝语义)
        memcpy(&Out.GrabConfig, &G->Configuration, sizeof(SDK::FEvilNunInteractStructure));
        // 快照中的 FString 必须清零: memcpy 只拷贝 Data 指针, 快照与原版共享字符串缓冲
        // 会在对象销毁时 double-free / 使用悬垂指针 — 崩溃根源, 绝不保留
        memset(&Out.GrabConfig.InteractText_2_99BB7E9440B8584BEC3058A99FEB8087, 0, sizeof(SDK::FString));
        Out.Mesh = G->Mesh;
        Out.Material = G->Material;
        Out.Scale = G->Scale;
        Out.GrabbedScale = G->GrabbedScale;
        Out.GrabbedLocationOffset = G->GrabbedLocationOffset;
        Out.GrabbedRotationOffset = G->GrabbedRotationOffset;
        Out.DropDistance = G->DropDistance;
        Out.DropSpaceScale = G->DropSpaceScale;
        Out.DropImpulseScale = G->DropImpulseScale;
        Out.GrabSound = G->GrabSound;
        Out.IsBunkerKey = G->IsBunkerKey;
        Out.IsDynamite = G->IsDynamite;
        Out.ObjectKind = (uint8_t)G->ObjectKind;
        Out.BeginsAwake = G->BeginsAwake;
        Out.TrueChargedDoll = G->TrueChargedDoll;
        Out.CollideWithPlayer = G->CollideWithPlayer;
        // 放入目标容器: 场景交互判定 "手中道具->NestingActor == 对准的Actor" 的核心凭据
        // (指针 + 索引成对捕获, 供跨关卡/已消耗场景的存活校验)
        Out.NestingActor = IsValidUObject(G->NestingActor) ? G->NestingActor : nullptr;
        Out.NestingActorIndex = Out.NestingActor ? G->NestingActor->Index : -1;
        Out.ReespawnLocation = G->ReespawnLocation;
        for (int32_t k = 0; k < G->PossibleSpawnLocations.Num(); k++)
        {
            auto* Loc = G->PossibleSpawnLocations[k];
            if (IsValidUObject(Loc))
            {
                Out.PossibleSpawnLocations.push_back(Loc);
                Out.PossibleSpawnLocationIndexes.push_back(Loc->Index);
            }
        }
    }
    else
    {
        auto* P = static_cast<SDK::APhysGrabObject_C*>(Actor);
        memcpy(&Out.PhysConfig, &P->InteractConfigVars, sizeof(SDK::FInteractConfigStruct));
        // 嵌套 FString 同样清零 (防销毁时 double-free, 见上方 Grab 分支注释)
        // 注意 ShowMessageStruct 内有两个 FString: Begining(0x30) 和 End(0x40) — 都要清
        memset(&Out.PhysConfig.ShowMessageStruct_20_D78F961244E3046B1D3F2AB2E7309884
                    .ExtraInteractionTextBegining_25_337B2BFE47CFCD62D4D2E5AFCA94FD9D,
               0, sizeof(SDK::FString));
        memset(&Out.PhysConfig.ShowMessageStruct_20_D78F961244E3046B1D3F2AB2E7309884
                    .ExtraInteractionTextEnd_26_0C04AAB24DCDD30B65C6E1898287ABA9,
               0, sizeof(SDK::FString));
        Out.Mesh = P->PhysicsMesh;
        Out.Material = P->MeshMaterial;
        Out.Scale = P->Scale;
        Out.GrabRotation = P->GrabRotation;
        Out.OrientObjectToCamera = P->OrientObjectToCamera;
        Out.StartAwaken = P->StartAwaken;
        Out.EnableGravity = P->EnableGravity;
        Out.FloatsInWater = P->FloatsInWater;
        Out.MassScale = P->MassScale;
        Out.MaxHoldDistance = P->MaxHoldDistance;
        Out.MaxVelocity = P->MaxVelocity;
        Out.CollideWithPlayer = P->CollideWithPlayer;
    }

    // 实例级属性通用捕获 (BreakPieces 碎片网格等手挑清单之外的状态)
    CaptureInstanceProps(Actor, Out);
}

// 文本规范化: 去首尾空白; "Grab" 是开发占位交互文本 (大量实例共用), 视为无效
static std::string NormalizeItemText(std::string S)
{
    size_t B = S.find_first_not_of(" \t\r\n");
    if (B == std::string::npos)
        return "";
    size_t E = S.find_last_not_of(" \t\r\n");
    S = S.substr(B, E - B + 1);
    if (_stricmp(S.c_str(), "Grab") == 0)
        return "";
    return S;
}

// Mesh 名美化 (仅显示用): 去掉重复的 SM_MERGED_ 打包前缀
static std::string PrettyMeshName(const std::string& M)
{
    std::string R = M;
    while (R.rfind("SM_MERGED_", 0) == 0)
        R = R.substr(10);
    return R;
}

// 扫描 GObjects 收集物品实例, 合并进 s_ItemClasses (增量积累 + 刷新已有快照自愈悬垂引用)
static void ScanItemsInternal()
{
    auto* GrabBase = SDK::ANunGrabObject_C::StaticClass();
    auto* PhysBase = SDK::APhysGrabObject_C::StaticClass();
    auto* Arr = SDK::UObject::GObjects.GetTypedPtr();
    if (!GrabBase || !PhysBase || !Arr)
        return;

    const int Num = Arr->Num();
    int instanceCount = 0;
    const size_t Before = s_ItemClasses.size();

    // 清理已销毁的克隆记录 (无解引用校验 — 克隆体随关卡销毁, 页面可能已反映射)
    s_CloneActors.erase(std::remove_if(s_CloneActors.begin(), s_CloneActors.end(),
        [](const ObjRef& R) { return !R.Alive(); }), s_CloneActors.end());

    // 实例名映射: 上一轮整体转存, 本轮重建 (克隆不参与匹配, 从上一轮带过来)
    s_InstanceToEntryPrev = std::move(s_InstanceToEntry);
    s_InstanceToEntry.clear();

    // 每轮扫描用 matched 标记条目是否遇到存活原版实例
    std::vector<bool> Matched(s_ItemClasses.size(), false);

    for (int i = 0; i < Num; i++)
    {
        SDK::UObject* Obj = Arr->GetByIndex(i);
        if (!Obj || !Obj->Class)
            continue;
        // 跳过半销毁对象 (Destroy 后 GC 前): 其组件/字符串内存可能已被释放
        if (!IsUsable(Obj))
            continue;

        bool IsGrab = Obj->IsA(GrabBase);
        bool IsPhys = !IsGrab && Obj->IsA(PhysBase);
        if (!IsGrab && !IsPhys)
            continue;

        // 跳过 CDO (类默认对象, 无实际配置)
        if (Obj->IsDefaultObject())
            continue;

        auto* Actor = static_cast<SDK::AActor*>(Obj);

        // 跳过我们自己生成的克隆 — 不能让它顶替原版成为模板 (克隆无场景交互能力)
        bool IsClone = false;
        for (auto& C : s_CloneActors)
            if (C.Ptr == Actor) { IsClone = true; break; }
        if (IsClone)
        {
            // 克隆不参与模板匹配, 但保留其物品条目映射 (ESP 显示名)
            auto It = s_InstanceToEntryPrev.find(Actor);
            if (It != s_InstanceToEntryPrev.end())
                s_InstanceToEntry.emplace(Actor, It->second);
            continue;
        }

        instanceCount++;

        std::string MeshName;
        std::string TextName;   // 游戏内名称 (交互文本 / MyselfName)
        int KindVal = 0;

        if (IsGrab)
        {
            auto* G = static_cast<SDK::ANunGrabObject_C*>(Obj);
            MeshName = IsValidUObject(G->Mesh) ? G->Mesh->GetName() : "";
            TextName = NormalizeItemText(FStringToStr(G->Configuration.InteractText_2_99BB7E9440B8584BEC3058A99FEB8087));
            KindVal = (int)G->ObjectKind;
        }
        else
        {
            auto* P = static_cast<SDK::APhysGrabObject_C*>(Obj);
            MeshName = IsValidUObject(P->PhysicsMesh) ? P->PhysicsMesh->GetName() : "";
            TextName = NormalizeItemText(FStringToStr(P->MyselfName));
        }

        if (MeshName.empty() && TextName.empty())
            continue; // Mesh 和名称全空, 无法识别

        // 去重: 基键 = Mesh+功能 (同 Mesh 不同真名 = 不同道具, 保留区分);
        // 全键 = 基键+真名。无名实例 (占位文本/空) 按基键归并, 不再产生第二条
        const std::string BaseKey = MeshName + "#" + std::to_string(KindVal);
        const std::string FullKey = BaseKey + "#" + TextName;

        // 1) 全键精确命中 → 刷新 (自愈悬垂引用)
        bool Exists = false;
        for (size_t k = 0; k < s_ItemClasses.size(); k++)
        {
            if (s_ItemClasses[k].DedupKey == FullKey)
            {
                Matched[k] = true;
                CaptureItemConfig(Actor, IsGrab, s_ItemClasses[k].Snap);
                s_ItemClasses[k].Template = Actor;
                s_ItemClasses[k].TemplateIndex = Actor->Index;
                s_ItemClasses[k].Consumed = false;
                s_InstanceToEntry.emplace(Actor, k);
                Exists = true;
                break;
            }
        }
        if (Exists)
            continue;

        // 同基键的既有条目统计 (无名条目 = 键尾正好是 "#", 即全键在文本为空时构建)
        int BaseCount = 0, BaseUnnamedIdx = -1;
        int BaseNamedIdx = -1;
        for (size_t k = 0; k < s_ItemClasses.size(); k++)
        {
            const std::string& DK = s_ItemClasses[k].DedupKey;
            if (DK.size() >= BaseKey.size() + 1
                && DK.compare(0, BaseKey.size() + 1, BaseKey + "#") == 0)
            {
                BaseCount++;
                if (DK.size() == BaseKey.size() + 1)
                    BaseUnnamedIdx = (int)k;
                else
                    BaseNamedIdx = (int)k;
            }
        }

        if (TextName.empty())
        {
            // 无名实例: 基键下已有 1 条 → 并入刷新 (不新建, 消灭 "Grab+真名" 双条目)
            if (BaseCount == 1)
            {
                const int k = BaseUnnamedIdx >= 0 ? BaseUnnamedIdx : BaseNamedIdx;
                Matched[k] = true;
                // 仅当目标条目也无名时刷新快照 (防无名实例的空配置覆盖有名快照)
                if (BaseUnnamedIdx >= 0)
                    CaptureItemConfig(Actor, IsGrab, s_ItemClasses[k].Snap);
                s_ItemClasses[k].Template = Actor;
                s_ItemClasses[k].TemplateIndex = Actor->Index;
                s_ItemClasses[k].Consumed = false;
                s_InstanceToEntry.emplace(Actor, (size_t)k);
                continue;
            }
            if (BaseCount > 1)
                continue; // 基键下多个有名条目 (同 Mesh 不同道具), 无名实例无法归属 — 跳过

            // 基键下 0 条: 无名也无 Tags = 开发占位 (Cone/Cube 等), 生成无意义 — 跳过
            if (Actor->Tags.Num() == 0)
                continue;
        }
        else
        {
            // 有名实例 + 基键下存在无名条目 → 升级该条目 (补名, 不新建)
            if (BaseUnnamedIdx >= 0)
            {
                ItemClassInfo& E = s_ItemClasses[BaseUnnamedIdx];
                Matched[BaseUnnamedIdx] = true;
                CaptureItemConfig(Actor, IsGrab, E.Snap);
                E.Name = TextName;
                E.MeshName = MeshName;
                E.DedupKey = FullKey;
                E.Template = Actor;
                E.TemplateIndex = Actor->Index;
                E.Consumed = false;
                s_InstanceToEntry.emplace(Actor, (size_t)BaseUnnamedIdx);
                ModMenu::Log("[Items] ~ %s [补名, 原占位条目] Mesh=%s", TextName.c_str(), MeshName.c_str());
                continue;
            }
        }

        ItemClassInfo Info;
        Info.Cls = Obj->Class;
        Info.Name = !TextName.empty() ? TextName : PrettyMeshName(MeshName);
        Info.MeshName = MeshName;
        Info.DedupKey = FullKey;
        Info.Group = IsGrab ? Lang::kGroupTask : Lang::kGroupPhys;
        Info.Template = Actor;
        Info.TemplateIndex = Actor->Index;
        CaptureItemConfig(Actor, IsGrab, Info.Snap);

        // 仅记录新增项 (刷新不刷屏); 注意在 move 之前读取字段
        ModMenu::Log("[Items] + %s [%s] Mesh=%s Tags=%d", Info.Name.c_str(), Info.Group, MeshName.c_str(), Actor->Tags.Num());
        s_ItemClasses.push_back(std::move(Info));
        Matched.push_back(true);
        s_InstanceToEntry.emplace(Actor, s_ItemClasses.size() - 1);
    }

    // 未遇到存活原版实例且模板已销毁的条目 → 标记已消耗 (只剩克隆/无实例)
    // (无解引用校验: 模板可能已被 GC 释放, 页面可能已反映射)
    for (size_t k = 0; k < s_ItemClasses.size() && k < Matched.size(); k++)
        if (!Matched[k] && !ObjRef{ s_ItemClasses[k].Template, s_ItemClasses[k].TemplateIndex }.Alive())
            s_ItemClasses[k].Consumed = true;

    s_ItemClassesScanned = true;
    s_LastItemScanTick = GetTickCount64();
    ValidateDoorRedirects();
    ModMenu::Log("[Items] 扫描: GObjects %d, 实例 %d, 新增 %zu, 总计 %zu",
                 Num, instanceCount, s_ItemClasses.size() - Before, s_ItemClasses.size());
}

const std::vector<ItemClassInfo>& GetAllItemClasses()
{
    if (!s_ItemClassesScanned)
        ScanItemsInternal();
    return s_ItemClasses;
}

void RescanItemClasses()
{
    s_ItemClasses.clear();
    s_ItemClassesScanned = false;
    s_InstanceToEntry.clear();
    s_InstanceToEntryPrev.clear();
    ScanItemsInternal();
}

const char* GetInstanceDisplayName(SDK::UObject* O)
{
    auto It = s_InstanceToEntry.find(O);
    if (It == s_InstanceToEntry.end() || It->second >= s_ItemClasses.size())
        return nullptr;
    return s_ItemClasses[It->second].Name.c_str();
}

int ForceLoadStreamingLevels()
{
    auto* W = GetWorld();
    if (!W || !IsUsable(W))
        return -1;

    int Triggered = 0;
    auto& Levels = W->StreamingLevels;
    for (int i = 0; i < Levels.Num(); i++)
    {
        SDK::ULevelStreaming* LS = Levels[i];
        if (!LS || !IsUsable(LS))
            continue;
        // 只触发未加载/未显示的; 已加载的直接跳过 (避免打断加载中状态)
        if (!LS->IsLevelLoaded() || !LS->IsLevelVisible())
        {
            LS->SetShouldBeLoaded(true);
            LS->SetShouldBeVisible(true);
            Triggered++;
        }
    }
    ModMenu::Log("[Items] 强制加载流式子关卡: %d/%d 个已触发 (随后由定期扫描自动收录新道具)",
                 Triggered, Levels.Num());
    return Triggered;
}

void DumpAllItemInstances()
{
    auto* GrabBase = SDK::ANunGrabObject_C::StaticClass();
    auto* PhysBase = SDK::APhysGrabObject_C::StaticClass();
    auto* Arr = SDK::UObject::GObjects.GetTypedPtr();
    if (!GrabBase || !PhysBase || !Arr)
        return;
    const int Num = Arr->Num();
    int GrabN = 0, PhysN = 0, SkipN = 0;
    ModMenu::Log("[Dump] ==== 物品实例全量清单 (%d GObjects) ====", Num);
    for (int i = 0; i < Num; i++)
    {
        SDK::UObject* Obj = Arr->GetByIndex(i);
        if (!Obj || !Obj->Class || !IsUsable(Obj) || Obj->IsDefaultObject())
            continue;
        bool IsGrab = Obj->IsA(GrabBase);
        bool IsPhys = !IsGrab && Obj->IsA(PhysBase);
        if (!IsGrab && !IsPhys)
            continue;
        // 跳过我们的克隆
        bool IsClone = false;
        for (auto& C : s_CloneActors)
            if (C.Ptr == Obj) { IsClone = true; break; }
        if (IsClone) { SkipN++; continue; }

        auto* Actor = static_cast<SDK::AActor*>(Obj);
        std::string MeshName, TextName;
        if (IsGrab)
        {
            auto* G = static_cast<SDK::ANunGrabObject_C*>(Obj);
            MeshName = IsValidUObject(G->Mesh) ? G->Mesh->GetName() : "";
            TextName = FStringToStr(G->Configuration.InteractText_2_99BB7E9440B8584BEC3058A99FEB8087);
            GrabN++;
        }
        else
        {
            auto* P = static_cast<SDK::APhysGrabObject_C*>(Obj);
            MeshName = IsValidUObject(P->PhysicsMesh) ? P->PhysicsMesh->GetName() : "";
            TextName = FStringToStr(P->MyselfName);
            PhysN++;
        }
        ModMenu::Log("[Dump] %s | Actor=%s | Mesh=%s | Text='%s' | Tags=%d",
                     IsGrab ? "Grab" : "Phys", Obj->GetName().c_str(),
                     MeshName.c_str(), TextName.c_str(), Actor->Tags.Num());
    }
    ModMenu::Log("[Dump] ==== 共 Grab %d, Phys %d (跳过克隆 %d) — 对照列表可定位缺失/重复 ====",
                 GrabN, PhysN, SkipN);
}

// ============ 传送点 / 关卡 ============
static std::vector<TeleportPoint> s_TeleportPoints;
static ULONGLONG s_TPScanTick = 0;
static SDK::UWorld* s_TPScanWorld = nullptr;

const std::vector<TeleportPoint>& GetTeleportPoints()
{
    auto* W = GetWorld();
    if (!W || !IsUsable(W))
    {
        s_TeleportPoints.clear();
        return s_TeleportPoints;
    }
    // 同一世界只扫一次 (路点/分类是关卡静态数据; 也避免 5 秒重扫刷日志)。
    // 扫到 0 个 (可能加载中) → 10 秒后允许重试
    if (s_TPScanWorld == W
        && (!s_TeleportPoints.empty() || GetTickCount64() - s_TPScanTick < 10000))
        return s_TeleportPoints;
    s_TPScanWorld = W;
    s_TPScanTick = GetTickCount64();
    s_TeleportPoints.clear();
    auto* Level = W->PersistentLevel;
    if (!Level || !IsUsable(Level))
        return s_TeleportPoints;

    // 1) 房间/场景: 游戏自带修女导航路点 (ANunNavigationWaypoint_C)。名称来源 =
    //    Category 枚举 (EWaypointCategory) 的 DisplayNameMap — UserDefinedEnum
    //    里开发者起的官方显示名 (路点 TextRender 标注 1012 版实测全是 'Default'
    //    占位, 不可用); 官方名经 s_RoomKeys 关键词映射成中文。
    //    聚合: 按官方名分组 (同名分类合并) → 2D 15m 单链分段 (忽略楼层差) →
    //    段中心 ≤50m 并段 (同房间被门洞断开), 多实例保留 "厨房 1/厨房 2";
    //    落点 = 成员路点位置中位数 (路点在导航网格上, 不卡墙) +120cm 胶囊半高。
    //    同一世界只扫一次; 组日志含分类号/Space 分布/坐标样本供调优。
    {
        // 分类官方名 → 中文名 (2026-10-02 日志实证 26 个官方名, 顺序敏感:
        // 长词在前防子串误命中, 如 churchbasement 必须在 church 之前)
        static const struct { const char* Key; const char* CN; } s_RoomKeys[] = {
            { "churchbasement",    "教堂地下室" },
            { "church",            "教堂" },
            { "laundryupperfloor", "洗衣房二楼" },
            { "laundrymainfloor",  "洗衣房一楼" },
            { "hiddenlaundry",     "密藏洗衣房" },
            { "laundry",           "洗衣房" },
            { "schoolupperfloor",  "学校二楼" },
            { "schoolmainfloor",   "学校一楼" },
            { "school",            "学校" },
            { "gymtop",            "体育馆顶部" },
            { "gymrooms",          "体育馆侧室" },
            { "gymmain",           "体育馆大厅" },
            { "gym",               "体育馆" },
            { "fountaingarden",    "喷泉花园" },
            { "statuegarden",      "雕像花园" },
            { "garden",            "花园" },
            { "nunbuilding_floor_0", "修女楼一层" },
            { "nunbuilding_floor_1", "修女楼二层" },
            { "nunbuilding_floor_2", "修女楼三层" },
            { "nunbuilding_floor_3", "修女楼四层" },
            { "nunbuilding_floor_4", "修女楼五层" },
            { "nunbuilding_exterior", "修女楼外围" },
            { "nunbuilding",          "修女楼" },
            { "boilerroom",        "锅炉房" },
            { "laberynth",         "迷宫" },
            { "labyrinth",         "迷宫" },
            { "bedrooms",          "卧室" },
            { "attic",             "阁楼" },
            { "rooftop",           "屋顶" },
            { "bunker",            "地堡" },
            { "parking",           "停车场" },
            { "dirt_road",         "土路" },
            { "dirtroad",          "土路" },
        };
        auto LowerOf = [](const std::string& S) {
            std::string L;
            L.reserve(S.size());
            for (char C : S)
                L += (char)tolower((unsigned char)C);
            return L;
        };
        constexpr size_t NK = sizeof(s_RoomKeys) / sizeof(s_RoomKeys[0]);

        // 路点收集: 类 == ANunNavigationWaypoint_C → Actor / 位置 / Category /
        // Space。同一 BP 类的 UClass* 是同一对象, 指针直接比对;
        // StaticClass() 按名字查类, 未加载时为 null
        auto* WPCls = SDK::ANunNavigationWaypoint_C::StaticClass();
        std::vector<SDK::AActor*> WPs;
        std::vector<SDK::FVector> WPPos;
        std::vector<int> WPCat;
        std::vector<int> WPSpace;
        if (WPCls)
        {
            for (int i = 0; i < Level->Actors.Num(); i++)
            {
                SDK::AActor* A = Level->Actors[i];
                if (!A || !IsUsable(A) || A->Class != WPCls)
                    continue;
                auto* WP = reinterpret_cast<SDK::ANunNavigationWaypoint_C*>(A);
                WPs.push_back(A);
                WPPos.push_back(A->K2_GetActorLocation());
                WPCat.push_back((int)WP->Category);
                WPSpace.push_back((int)WP->Space);
            }
        }
        else
        {
            ModMenu::Log("[TP] 警告: NunNavigationWaypoint_C 类未加载");
        }
        ModMenu::Log("[TP] 导航路点: %zu 个", WPs.size());

        // 重名去重 (不同房间常有同名 "Floor" 地砖)
        std::unordered_set<std::string> UsedNames;
        auto AddRoom = [&](const std::string& Name, SDK::FVector Pos)
        {
            std::string N = Name.empty() ? "未命名点" : Name;
            int Suffix = 2;
            while (!UsedNames.insert(N).second)
                N = Name + " #" + std::to_string(Suffix++);
            TeleportPoint TP;
            TP.Name = N;
            TP.IsRoom = true;
            TP.Pos = Pos;
            TP.Pos.Z += 120.0; // 胶囊半高: 落点在地板面上方 20 余 cm, 自由落体着地
            s_TeleportPoints.push_back(std::move(TP));
        };

        // ---- 分类官方名: Category/Space 枚举是 UserDefinedEnum, DisplayNameMap
        //      存开发者起的显示名 (路点 TextRender 标注 1012 版实测全是 'Default'
        //      占位, 不可用)。一次 GObjects 全扫找出两个枚举对象
        std::unordered_map<int, std::string> CatNames, SpaceNames;
        {
            SDK::UObject* CatObj = nullptr;
            SDK::UObject* SpaceObj = nullptr;
            if (auto* Arr = SDK::UObject::GObjects.GetTypedPtr())
            {
                const int Num = Arr->Num();
                for (int i = 0; i < Num && !(CatObj && SpaceObj); i++)
                {
                    SDK::UObject* O = Arr->GetByIndex(i);
                    if (!O || !O->Class || !IsUsable(O->Class)
                        || O->Class->GetName() != "UserDefinedEnum")
                        continue;
                    const std::string N = O->GetName();
                    if (N == "WaypointCategory")
                        CatObj = O;
                    else if (N == "WaypointSpaceKind")
                        SpaceObj = O;
                }
            }
            auto ReadMap = [&](SDK::UObject* Obj, std::unordered_map<int, std::string>& Out)
            {
                if (!Obj || !IsUsable(Obj))
                    return;
                auto& M = reinterpret_cast<SDK::UUserDefinedEnum*>(Obj)->DisplayNameMap;
                if (M.Num() <= 0 || M.Num() > 200)
                    return;
                for (auto It = begin(M); It != end(M); ++It)
                {
                    const std::string Key = It->Key().ToString(); // "NewEnumeratorN"
                    if (Key.rfind("NewEnumerator", 0) != 0)
                        continue;
                    Out[atoi(Key.c_str() + 13)] =
                        FStringToStr(SDK::UKismetTextLibrary::Conv_TextToString(It->Value()));
                }
            };
            ReadMap(CatObj, CatNames);
            ReadMap(SpaceObj, SpaceNames);
            ModMenu::Log("[TP] 分类官方名 %d 项:", (int)CatNames.size());
            for (auto& KV : CatNames)
                ModMenu::Log("[TP]   %d = '%s'", KV.first, KV.second.c_str());
            ModMenu::Log("[TP] Space 官方名 %d 项:", (int)SpaceNames.size());
            for (auto& KV : SpaceNames)
                ModMenu::Log("[TP]   %d = '%s'", KV.first, KV.second.c_str());
        }

        // 官方名可用于命名/跨分类合并 ('Default'/'Room' 是占位泛名, 不可用)
        auto Meaningful = [&](const std::string& S) {
            if (S.empty() || S.rfind("NewEnumerator", 0) == 0)
                return false;
            const std::string L = LowerOf(S);
            return L != "default" && L != "room";
        };

        // 官方名 → 路点索引分组 (同名分类合并; 无官方名按分类号, 键 "#N")
        std::unordered_map<std::string, std::vector<int>> ByLabel;
        std::unordered_map<std::string, std::vector<int>> LabelCats;
        for (size_t i = 0; i < WPs.size(); i++)
        {
            std::string Lab;
            auto It = CatNames.find(WPCat[i]);
            if (It != CatNames.end() && Meaningful(It->second))
                Lab = It->second;
            if (Lab.empty())
                Lab = "#" + std::to_string(WPCat[i]);
            ByLabel[Lab].push_back((int)i);
            auto& Cs = LabelCats[Lab];
            if (std::find(Cs.begin(), Cs.end(), WPCat[i]) == Cs.end())
                Cs.push_back(WPCat[i]);
        }

        auto Med = [](std::vector<double> V) {
            size_t M = V.size() / 2;
            std::nth_element(V.begin(), V.begin() + M, V.end());
            return V[M];
        };
        // 路点 2D 单链聚类 (忽略 Z, 楼层差不拆同房间): ≤1500cm 链归成段
        // (UE 坐标单位是厘米! 阈值必须按 cm 传, 1012/1211 版误传 15.0 = 15cm,
        //  聚类从未生效 → 每路点一点全是重复)
        auto ClusterWPs = [&](const std::vector<int>& In, double Link)
            -> std::vector<std::vector<int>>
        {
            std::vector<std::vector<int>> Out;
            const double L2 = Link * Link;
            std::vector<int> Cid(In.size(), -1);
            for (size_t i = 0; i < In.size(); i++)
            {
                if (Cid[i] >= 0)
                    continue;
                std::vector<int> Members{ In[i] };
                std::vector<size_t> Queue{ i };
                Cid[i] = (int)Out.size();
                for (size_t Q = 0; Q < Queue.size(); Q++)
                {
                    const auto& P = WPPos[In[Queue[Q]]];
                    for (size_t K = 0; K < In.size(); K++)
                    {
                        if (Cid[K] >= 0)
                            continue;
                        const auto& Qp = WPPos[In[K]];
                        const double dx = Qp.X - P.X;
                        const double dy = Qp.Y - P.Y;
                        if (dx * dx + dy * dy <= L2)
                        {
                            Cid[K] = Cid[Queue[Q]];
                            Queue.push_back(K);
                            Members.push_back(In[K]);
                        }
                    }
                }
                Out.push_back(std::move(Members));
            }
            return Out;
        };
        // 段/组落点 = 成员路点位置中位数 (路点必在导航网格上, 不会卡墙)
        auto MedPoint = [&Med, &WPPos](const std::vector<int>& Ms) -> SDK::FVector
        {
            std::vector<double> XS, YS, ZS;
            XS.reserve(Ms.size()); YS.reserve(Ms.size()); ZS.reserve(Ms.size());
            for (int Idx : Ms)
            {
                XS.push_back(WPPos[Idx].X);
                YS.push_back(WPPos[Idx].Y);
                ZS.push_back(WPPos[Idx].Z);
            }
            return SDK::FVector{ (float)Med(XS), (float)Med(YS), (float)Med(ZS) };
        };

        for (auto& KV : ByLabel)
        {
            auto& Idxs = KV.second;
            if (Idxs.empty())
                continue;
            const std::string& Lab = KV.first;
            // 中文名: 官方名 → s_RoomKeys 中文; 未匹配保留原文;
            // 无官方名 ("#N") 则 "场景 N"
            std::string CN;
            if (Lab[0] != '#')
            {
                const std::string Low = LowerOf(Lab);
                for (size_t K = 0; K < NK; K++)
                    if (Low.find(s_RoomKeys[K].Key) != std::string::npos)
                    { CN = s_RoomKeys[K].CN; break; }
                if (CN.empty())
                    CN = Lab;
            }
            else
                CN = "场景 " + Lab.substr(1);

            // 大跨度分组 2D 单链分段 (15m=1500cm), 段中心 ≤50m=5000cm 并段 (同房间被门洞
            // 断开); 真正的多实例房间间距远 → "厨房 1/厨房 2"
            auto Segs = ClusterWPs(Idxs, 1500.0);
            std::vector<SDK::FVector> Centers;
            for (auto& S : Segs)
                Centers.push_back(MedPoint(S));
            std::vector<int> Par(Segs.size());
            for (size_t i = 0; i < Segs.size(); i++)
                Par[i] = (int)i;
            auto Find = [&](int X) { while (Par[X] != X) X = Par[X] = Par[Par[X]]; return X; };
            for (size_t i = 0; i < Segs.size(); i++)
                for (size_t j = i + 1; j < Segs.size(); j++)
                {
                    const double dx = Centers[i].X - Centers[j].X;
                    const double dy = Centers[i].Y - Centers[j].Y;
                    if (dx * dx + dy * dy <= 5000.0 * 5000.0)
                        Par[Find((int)j)] = Find((int)i);
                }
            std::vector<std::vector<int>> Merged(Segs.size());
            for (size_t i = 0; i < Segs.size(); i++)
                for (int Idx : Segs[i])
                    Merged[Find((int)i)].push_back(Idx);
            int Seg = 0, SegTotal = 0;
            for (auto& Mm : Merged)
                if (!Mm.empty())
                    SegTotal++;
            for (auto& Mm : Merged)
            {
                if (Mm.empty())
                    continue;
                Seg++;
                AddRoom(SegTotal == 1 ? CN : CN + " " + std::to_string(Seg), MedPoint(Mm));
            }

            // 一次性诊断: 分类号 / Space 分布 / 坐标样本 (供命名与阈值调优)
            char Buf[96];
            std::string Cats;
            {
                auto& Cs = LabelCats[Lab];
                std::sort(Cs.begin(), Cs.end());
                for (int C : Cs)
                {
                    snprintf(Buf, sizeof(Buf), "%s%d", Cats.empty() ? "" : ",", C);
                    Cats += Buf;
                }
            }
            std::unordered_map<int, int> SpCnt;
            for (int Idx : Idxs)
                SpCnt[WPSpace[Idx]]++;
            std::string Spaces;
            for (auto& SC : SpCnt)
            {
                snprintf(Buf, sizeof(Buf), "%s%d×%d", Spaces.empty() ? "" : " ", SC.first, SC.second);
                Spaces += Buf;
            }
            std::string Samp;
            for (size_t S = 0; S < Idxs.size() && S < 5; S++)
            {
                snprintf(Buf, sizeof(Buf), "%s(%.0f,%.0f,%.0f)", Samp.empty() ? "" : " ",
                         WPPos[Idxs[S]].X, WPPos[Idxs[S]].Y, WPPos[Idxs[S]].Z);
                Samp += Buf;
            }
            ModMenu::Log("[TP] 组 '%s' (%zu 路点, 分类 %s, Space %s) → %d 点, 名 '%s', 样本:%s",
                         Lab.c_str(), Idxs.size(), Cats.c_str(), Spaces.c_str(),
                         SegTotal, CN.c_str(), Samp.c_str());
        }
        ModMenu::Log("[TP] 传送点共 %zu 个 (按导航路点官方名聚合)",
                     s_TeleportPoints.size());
    }

    // 1b) 传统流式子关卡 (其他地图可能使用, MainMap 为 WorldPartition 无此项)
    auto& Levels = W->StreamingLevels;
    if (Levels.Num() > 0)
        ModMenu::Log("[TP] 扫描: StreamingLevels=%d", Levels.Num());
    for (int i = 0; i < Levels.Num(); i++)
    {
        SDK::ULevelStreaming* LS = Levels[i];
        if (!LS || !IsUsable(LS))
            continue;
        std::string Pkg = LS->PackageNameToLoad.ToString();
        // 去路径与主地图前缀: "/Game/Maps/EvilNun-MainMap_XYZ" → "XYZ"
        size_t Slash = Pkg.find_last_of("/\\");
        if (Slash != std::string::npos)
            Pkg = Pkg.substr(Slash + 1);
        const std::string Prefix = "EvilNun-MainMap_";
        if (Pkg.rfind(Prefix, 0) == 0)
            Pkg = Pkg.substr(Prefix.size());
        if (Pkg.empty())
            continue;
        TeleportPoint TP;
        TP.Name = Pkg;
        TP.IsRoom = true;
        TP.SrcLevel = LS;
        // FTransform: Quat(0x10) + Translation(+0x10)
        TP.Pos = *(SDK::FVector*)((uint8_t*)&LS->LevelTransform + 0x10);
        TP.Pos.Z += 120.0; // 与房间落点一致: 防止胶囊原点半埋入地
        s_TeleportPoints.push_back(std::move(TP));
    }

    // 2) 出生点: PlayerStart (中文名, 多个自动 "#N" 序号, 与房间点同格式)
    if (auto* PSClass = SDK::APlayerStart::StaticClass())
    {
        std::unordered_set<std::string> UsedPS; // 名字去重 (每次扫描重建)
        for (int i = 0; i < Level->Actors.Num(); i++)
        {
            SDK::AActor* A = Level->Actors[i];
            if (!A || !IsUsable(A) || !A->IsA(PSClass))
                continue;
            TeleportPoint TP;
            TP.Name = "出生点";
            int Suffix = 2;
            while (!UsedPS.insert(TP.Name).second)
                TP.Name = "出生点 #" + std::to_string(Suffix++);
            TP.Pos = A->K2_GetActorLocation();
            s_TeleportPoints.push_back(std::move(TP));
        }
    }
    return s_TeleportPoints;
}

// 应用类型化配置到新实例 (对象指针先校验有效性)
static void ApplyItemConfig(SDK::AActor* Dest, const ItemConfigSnapshot& S, bool RestoreIdentity)
{
    if (Dest->IsA(SDK::ANunGrabObject_C::StaticClass()))
    {
        auto* G = static_cast<SDK::ANunGrabObject_C*>(Dest);
        // 分段拷贝, 跳过 FString 字段 (偏移 0x00-0x10 的 InteractText):
        // 克隆体的 FString (构造脚本已按 CDO 深拷贝分配) 完全不动 —
        // 任何形式的整体 memcpy 都会造成字符串缓冲共享 (销毁时 double-free)
        // 或覆盖掉构造分配 (悬垂/空文本引发游戏文本格式化崩溃)
        memcpy((char*)&G->Configuration + 0x10, (const char*)&S.GrabConfig + 0x10,
               sizeof(SDK::FEvilNunInteractStructure) - 0x10);
        if (IsValidUObject(S.Mesh))       G->Mesh = S.Mesh;
        if (IsValidUObject(S.Material))   G->Material = S.Material;
        if (IsValidUObject(S.GrabSound))  G->GrabSound = S.GrabSound;
        G->Scale = S.Scale;
        G->GrabbedScale = S.GrabbedScale;
        G->GrabbedLocationOffset = S.GrabbedLocationOffset;
        G->GrabbedRotationOffset = S.GrabbedRotationOffset;
        G->DropDistance = S.DropDistance;
        G->DropSpaceScale = S.DropSpaceScale;
        G->DropImpulseScale = S.DropImpulseScale;
        G->IsBunkerKey = S.IsBunkerKey;
        G->IsDynamite = S.IsDynamite;
        G->ObjectKind = (SDK::ESpecialObjectEnum)S.ObjectKind;
        G->BeginsAwake = S.BeginsAwake;
        G->TrueChargedDoll = S.TrueChargedDoll;
        G->CollideWithPlayer = S.CollideWithPlayer;
        // 放入目标容器 (幂等, 单指针直接覆盖)。快照指针可能因跨关卡/对象销毁失效 —
        // 用捕获时的 GObjects 索引做无解引用存活校验 (先 Alive 后 IsUsable, 顺序不可反:
        // 页面已反映射的悬垂指针连 VTable 都不能读), 失效置空防交互时解引用悬垂
        if (S.NestingActor && ObjRef{ S.NestingActor, S.NestingActorIndex }.Alive()
            && IsUsable(S.NestingActor))
            G->NestingActor = S.NestingActor;
        else
            G->NestingActor = nullptr;
        G->ReespawnLocation = S.ReespawnLocation;
        // 生成点数组: 不用 Add (新建实例数组通常无 slack, Add 会越界写坏堆!)
        // 由 SpawnObjectFromTemplate 的移交步骤统一 memcpy 处理
        // (快照中的 PossibleSpawnLocations 仅作展示/诊断用途)
    }
    else
    {
        auto* P = static_cast<SDK::APhysGrabObject_C*>(Dest);
        // 分段拷贝, 跳过两个嵌套 FString (ShowMessageStruct 内偏移 0x50-0x70:
        // ExtraInteractionTextBegining + ExtraInteractionTextEnd):
        // 克隆体自己的字符串分配完全不动 (见上方注释)
        memcpy(&P->InteractConfigVars, &S.PhysConfig, 0x50);
        memcpy((char*)&P->InteractConfigVars + 0x70, (const char*)&S.PhysConfig + 0x70,
               sizeof(SDK::FInteractConfigStruct) - 0x70);
        if (IsValidUObject(S.Mesh))     P->PhysicsMesh = S.Mesh;
        if (IsValidUObject(S.Material)) P->MeshMaterial = S.Material;
        P->Scale = S.Scale;
        P->GrabRotation = S.GrabRotation;
        P->OrientObjectToCamera = S.OrientObjectToCamera;
        P->StartAwaken = S.StartAwaken;
        P->EnableGravity = S.EnableGravity;
        P->FloatsInWater = S.FloatsInWater;
        P->MassScale = S.MassScale;
        P->MaxHoldDistance = S.MaxHoldDistance;
        P->MaxVelocity = S.MaxVelocity;
        P->CollideWithPlayer = S.CollideWithPlayer;
    }

    // 恢复 Tags 标签 (蓝图交互检查项; 仅数组有余量时可写)
    // 注意: 不能直接写 Name — FName 参与引擎哈希注册, 裸改会导致销毁时 RemoveFromHash 崩溃
    // Tags 不再走 Add (新建实例数组无 slack 时 Add 越界写坏堆 — 曾导致通关崩溃):
    // 任务道具 Tags 由 SpawnObjectFromTemplate 的移交步骤 memcpy 处理
}

bool IsTemplateAlive(const ItemClassInfo& Item)
{
    // 身份持有者 (最近一次转移到的克隆) 存活, 或原版仍存活 → 还能生成可交互道具
    // (无解引用校验: 菜单每帧调用, 模板随关卡销毁后页面可能已反映射)
    if (ObjRef{ Item.IdentityHolder, Item.HolderIndex }.Alive())
        return true;
    return !Item.Consumed && ObjRef{ Item.Template, Item.TemplateIndex }.Alive();
}

// ============ 身份字段移交 ============
// 场景交互判定走克隆体自身字段 (NestingActor 指针 + Tags 标签 + 事件委托),
// 全部为"克隆体持有指向场景的引用", 不改任何外部对象 — 外部引用 (关卡蓝图变量等)
// 继续指向原版, 原版被游戏消耗/销毁前始终有效, 不存在悬垂风险。
// (曾尝试全 GObjects 引用重定向: 克隆体被保险丝盒消耗销毁后,
//  被重定向的关卡蓝图变量在结局 FText 格式化时读到悬垂对象 → 崩溃, 已废弃)

// 引擎侧深拷贝一个数组属性 (UKismetArrayLibrary::SetArrayPropertyByName):
// 内部走 FScriptArrayHelper::AddValue → 引擎分配器扩容 + 按真实元素类型逐个拷贝,
// 是外部代码给引擎数组"扩容写入"的唯一安全通道 (SDK TArray::Add 无 slack 时静默失败,
// 手工扩容无法调用引擎分配器, 所有权移交会掏空原版 → 游戏逻辑读空壳道具崩溃)
static void DeepCopyArrayProp(SDK::UObject* Dest, SDK::FName PropName, const void* SrcArray)
{
    SDK::UKismetArrayLibrary::SetArrayPropertyByName(
        Dest, PropName, *reinterpret_cast<const SDK::TArray<SDK::int32>*>(SrcArray));
}

// 深拷贝身份数组字段 (Tags / PossibleSpawnLocations): 原版完全不动, 克隆体获得
// 与原版内容相同但内存独立的数组 — 双方销毁互不影响。
// 实现: 反射遍历 ONLY 用于取 FName (引擎函数按名查属性, SDK 的 FName 无字符串构造);
// 数据源一律用 SDK 成员直接偏移。不再读 FFieldClass::CastFlags 判属性类型 —
// 同一份代码在不同会话时而识别 Tags 时而跳过 (A/B/C 三轮日志: 失败→成功→失败),
// CastFlags 读取在该引擎上不稳定, Tags 拷贝静默失效 = 克隆体无交互凭据 = 道具不可用。
// 多播委托不复制: SetArrayPropertyByName 只接受 ArrayProperty, 对委托属性静默无效,
// 且场景交互凭据是 Tags + NestingActor, 委托非必需。
static void CopyMulticastDelegates(SDK::AActor* Old, SDK::AActor* New)
{
    if (!Old || !New || Old->Class != New->Class)
        return;

    for (SDK::UStruct* S = Old->Class; S; S = S->SuperStruct)
    {
        for (SDK::FField* F = S->ChildProperties; F; F = F->Next)
        {
            const std::string NameStr = F->Name.ToString();
            const void* Src = nullptr;
            if (NameStr == "Tags")
                Src = &Old->Tags;   // AActor 成员, SDK 偏移确定性
            else if (NameStr == "PossibleSpawnLocations"
                     && Old->IsA(SDK::ANunGrabObject_C::StaticClass()))
                Src = &static_cast<SDK::ANunGrabObject_C*>(Old)->PossibleSpawnLocations;
            if (!Src)
                continue;

            const auto* SrcArr = (const SDK::TArray<SDK::int32>*)Src;
            if (SrcArr->Num() <= 0)
                continue;

            // 同类对象成员偏移一致: 目标地址 = New + (Src - Old)
            auto* DstArr = (SDK::TArray<SDK::int32>*)((char*)New + ((uintptr_t)Src - (uintptr_t)Old));
            const int SrcNum = SrcArr->Num(), DstBefore = DstArr->Num();
            DeepCopyArrayProp(New, F->Name, Src);
            ModMenu::Log("[Spawn]   %s: 源 %d → 目标 %d → %d%s",
                         NameStr.c_str(), SrcNum, DstBefore, DstArr->Num(),
                         DstArr->Num() == SrcNum ? "" : " (拷贝未生效!)");
        }
    }
}

// 已消耗物品的身份恢复: 原版与全部克隆都被游戏消耗销毁后, 从生成时捕获的配置
// 快照重建交互身份 (Tags 标签 + 生成点数组; NestingActor 由 ApplyItemConfig 校验
// 写入) — 使"已消耗"的道具也能一直生成【可用】克隆。
// 安全性:
//   Tags 是值拷贝 (FName 索引全进程稳定), 任意时刻可回放;
//   Actor 指针 (生成点) 用捕获时的 GObjects 索引做无解引用存活校验 (先 Alive 后
//   IsUsable) — 同一关卡内场景 Actor 持续存活所以能恢复; 跨关卡残留指针被拒绝,
//   自动降级为仅 Tags (交互凭据的主要部分在 Tags, 容器判定另有 NestingActor)。
// 写入走 DeepCopyArrayProp (引擎分配器扩容), 与身份移交同一安全通道。
// 返回是否恢复了至少一项身份。
static bool RestoreIdentityFromSnapshot(SDK::AActor* New, const ItemClassInfo& Item)
{
    if (!New || !New->IsA(SDK::ANunGrabObject_C::StaticClass()))
        return false; // 物理杂物无交互身份
    auto* G = static_cast<SDK::ANunGrabObject_C*>(New);
    const ItemConfigSnapshot& S = Item.Snap;

    bool Any = false;
    if (!S.Tags.empty())
    {
        std::vector<SDK::FName> Values = S.Tags;
        SDK::TArray<SDK::FName> Src(Values.data(), (int32_t)Values.size(), (int32_t)Values.size());
        SDK::FName TagsName = SDK::BasicFilesImplUtils::StringToName(L"Tags");
        DeepCopyArrayProp(New, TagsName, &Src);
        ModMenu::Log("[Spawn]   Tags: 快照 %zu 个 → 目标 %d", S.Tags.size(), G->Tags.Num());
        Any = true;
    }
    if (!S.PossibleSpawnLocations.empty())
    {
        std::vector<SDK::AActor*> Locs;
        for (size_t i = 0; i < S.PossibleSpawnLocations.size(); i++)
        {
            auto* P = S.PossibleSpawnLocations[i];
            int32_t Idx = i < S.PossibleSpawnLocationIndexes.size()
                        ? S.PossibleSpawnLocationIndexes[i] : -1;
            // 无解引用存活校验 (悬垂指针的页面可能已反映射, 只查 GObjects 槽位)
            if (P && ObjRef{ P, Idx }.Alive() && IsUsable(P))
                Locs.push_back(P);
        }
        if (!Locs.empty())
        {
            SDK::TArray<SDK::AActor*> Src(Locs.data(), (int32_t)Locs.size(), (int32_t)Locs.size());
            SDK::FName LocName = SDK::BasicFilesImplUtils::StringToName(L"PossibleSpawnLocations");
            DeepCopyArrayProp(New, LocName, &Src);
            ModMenu::Log("[Spawn]   生成点: 快照 %zu → 存活 %zu → 目标 %d",
                         S.PossibleSpawnLocations.size(), Locs.size(),
                         G->PossibleSpawnLocations.Num());
            Any = true;
        }
    }
    return Any;
}

// ============ 门钥匙引用重定向 ============
// 门的交互凭据是 EvilNunDoorActor_C::SpecialKey — 逐实例配置的【对象指针】,
// 交互时引用比较 (手持物 == SpecialKey)。克隆体永远过不了比较 → 生成的钥匙
// 开不了门 (后花园钥匙实测)。修法: 身份移交时把指向身份持有者的门引用改指新
// 克隆。与曾致崩溃的全局引用重定向的区别: 只重定向门的 SpecialKey 一个字段,
// 且登记后由物品定期扫描校验 — 克隆被消耗销毁时把门恢复为原版 (仍存活) 或置空,
// 杜绝悬垂 (快照恢复路径用已死原版的【指针值】比较匹配门, 全程不解引用)。
struct DoorKeyRedirect
{
    SDK::AEvilNunDoorActor_C* Door; int32_t DoorIndex;
    SDK::AActor* OriginalKey;     int32_t OriginalKeyIndex;
    SDK::AActor* CloneKey;        int32_t CloneKeyIndex;
};
static std::vector<DoorKeyRedirect> s_DoorRedirects;

// From 可以是已销毁对象 (快照恢复路径) — 只做指针【值】比较, 绝不解引用
static void RedirectDoorKeys(SDK::AActor* From, SDK::AActor* To)
{
    if (!From || !To)
        return;
    auto* DoorCls = SDK::AEvilNunDoorActor_C::StaticClass();
    auto* W = GetWorld();
    if (!DoorCls || !W || !W->PersistentLevel)
        return;
    auto& Actors = W->PersistentLevel->Actors;
    for (int i = 0; i < Actors.Num(); i++)
    {
        auto* A = Actors[i];
        if (!A || !IsUsable(A) || !A->IsA(DoorCls))
            continue;
        auto* Door = (SDK::AEvilNunDoorActor_C*)A;
        if (Door->SpecialKey != From)
            continue;
        s_DoorRedirects.push_back({ Door, Door->Index, From, From->Index, To, To->Index });
        Door->SpecialKey = To;
        ModMenu::Log("[Spawn] 门钥匙重定向: [%s] SpecialKey %p → [%s]",
                     Door->GetName().c_str(), (void*)From, To->GetName().c_str());
    }
}

// 定期校验 (物品扫描时调用): 门或克隆任一死亡即清理 — 克隆被消耗时恢复原版
// (仍存活) 或置空, 保证门的 SpecialKey 永不悬垂
static void ValidateDoorRedirects()
{
    for (size_t k = 0; k < s_DoorRedirects.size(); )
    {
        auto& R = s_DoorRedirects[k];
        if (!ObjRef{ R.Door, R.DoorIndex }.Alive()
            || !ObjRef{ R.CloneKey, R.CloneKeyIndex }.Alive())
        {
            // 门还在但克隆已死 → 恢复; 门也没了 (换关卡) → 直接丢弃
            if (R.Door && ObjRef{ R.Door, R.DoorIndex }.Alive() && IsUsable(R.Door)
                && R.Door->SpecialKey == R.CloneKey)
            {
                SDK::AActor* Restore = nullptr;
                if (R.OriginalKey && ObjRef{ R.OriginalKey, R.OriginalKeyIndex }.Alive()
                    && IsUsable(R.OriginalKey))
                    Restore = R.OriginalKey;
                R.Door->SpecialKey = Restore;
                ModMenu::Log("[Spawn] 门钥匙重定向清理: [%s] SpecialKey → %s",
                             R.Door->GetName().c_str(), Restore ? "原版" : "空");
            }
            s_DoorRedirects.erase(s_DoorRedirects.begin() + k);
            continue;
        }
        k++;
    }
}

// 在指定位置克隆一个物品实例 (应用快照配置 + 强制网格)
static SDK::AActor* CloneItemActor(const ItemClassInfo& Item, const SDK::FVector& Loc, const SDK::FRotator& Rot)
{
    SDK::FTransform T;
    T.Rotation = SDK::FQuat{ 0.0, 0.0, 0.0, 1.0 };
    T.Translation = Loc;
    T.Scale3D = SDK::FVector{ 1.0, 1.0, 1.0 };

    auto* New = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
        s_World, Item.Cls, T,
        SDK::ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn, nullptr);
    if (!New)
    {
        ModMenu::Log("[Spawn] %s 失败: BeginDeferredActorSpawn 返回空", Item.Name.c_str());
        return nullptr;
    }

    // Finish 前应用配置 (构造脚本可见这些值, 如 Cube 网格由 Mesh 属性驱动)
    ApplyItemConfig(New, Item.Snap, false);

    SDK::AActor* Finished = SDK::UGameplayStatics::FinishSpawningActor(New, T);

    // 构造脚本可能按 CDO 重置字段, 生成完成后再应用一次 + 强制网格
    if (Finished)
    {
        ApplyItemConfig(Finished, Item.Snap, true);
        if (Finished->IsA(SDK::ANunGrabObject_C::StaticClass()))
        {
            auto* NG = static_cast<SDK::ANunGrabObject_C*>(Finished);
            if (NG->Cube && IsValidUObject(NG->Mesh))
                NG->Cube->SetStaticMesh(NG->Mesh);
            else
                ModMenu::Log("[Spawn] %s: 网格未应用 (Cube=%p Mesh=%p)",
                             Item.Name.c_str(), (void*)NG->Cube, (void*)NG->Mesh);
        }
        else
        {
            auto* PG = static_cast<SDK::APhysGrabObject_C*>(Finished);
            if (PG->Mesh && IsValidUObject(PG->PhysicsMesh))
                PG->Mesh->SetStaticMesh(PG->PhysicsMesh);
            else
                ModMenu::Log("[Spawn] %s: 网格未应用 (MeshComp=%p PhysicsMesh=%p)",
                             Item.Name.c_str(), (void*)PG->Mesh, (void*)PG->PhysicsMesh);
        }
        Finished->SetActorScale3D(Item.Snap.TemplateScale);
        // 实例级属性回放 (BreakPieces 碎片网格等): 必须在 FinishSpawning 之后 —
        // 构造脚本会按 CDO 重置字段, 先写会被覆盖
        ApplyInstanceProps(Finished, Item.Snap);
        // 登记克隆: 扫描刷新时跳过, 防止克隆顶替原版成为模板
        s_CloneActors.push_back(ObjRef::Make(Finished));
        // 克隆 → 物品条目映射 (ESP 显示名; Item 引用 s_ItemClasses 的元素)
        ptrdiff_t ItemIdx = &Item - s_ItemClasses.data();
        if (ItemIdx >= 0 && ItemIdx < (ptrdiff_t)s_ItemClasses.size())
            s_InstanceToEntry.emplace(Finished, (size_t)ItemIdx);

        // GC 止血: 运行时生成的 Actor 一出生就带 Async 内部标志 (0x4000000),
        // 被销毁后成 GC 僵尸拖住旧 World → 结算 Fatal。克隆体已在关卡 Actor 列表中
        // (GC 可达), 本体与组件 (同时创建) 一并清除, 回归正常 GC 生命周期。
        ModMenu::Log("[Spawn] %s 克隆: [%s] 内部标志=0x%X", Item.Name.c_str(),
                     Finished->GetName().c_str(), GetInternalFlags(Finished));
        ClearAsyncFlags(Finished, "生成");
        auto Comps = Finished->K2_GetComponentsByClass(SDK::UActorComponent::StaticClass());
        for (int i = 0; i < Comps.Num(); i++)
            if (Comps[i])
                ClearAsyncFlags(Comps[i], "生成");
    }
    return Finished;
}

SDK::AActor* SpawnObjectFromTemplate(const ItemClassInfo& Item, double Distance)
{
    if (!s_World || !s_Player || !IsValidUObject(Item.Cls))
    {
        ModMenu::Log("[Spawn] %s 失败: 前置无效 (World=%p Player=%p Cls=%p)",
                     Item.Name.c_str(), (void*)s_World, (void*)s_Player, (void*)Item.Cls);
        return nullptr;
    }

    // 生成位置: 玩家朝向前方 Distance 米, 抬高 50cm 防止卡进地面
    SDK::FVector Loc = s_Player->K2_GetActorLocation();
    SDK::FRotator Rot = s_Player->K2_GetActorRotation();
    double RadYaw = Rot.Yaw * 3.14159265358979323846 / 180.0;
    SDK::FVector Dest;
    Dest.X = Loc.X + cos(RadYaw) * Distance * 100.0;
    Dest.Y = Loc.Y + sin(RadYaw) * Distance * 100.0;
    Dest.Z = Loc.Z + 50.0;

    // 任务道具“身份转移”: 场景交互 (保险丝盒等) 通过引用比较判定道具, 克隆体无法通过判定;
    // 因此生成克隆体后, 把全游戏指向“身份持有者”的引用重定向到克隆体, 并复制事件委托 —
    // 克隆体成为可完整交互的道具, 原版留在原地, 关卡原貌不变
    const bool IsTaskItem = Lang::IsTaskGroup(Item.Group);
    SDK::AActor* Holder = nullptr;
    if (IsTaskItem)
    {
        if (ObjRef{ Item.IdentityHolder, Item.HolderIndex }.Alive())
            Holder = Item.IdentityHolder;               // 身份已在此前生成的克隆体上
        else if (!Item.Consumed && ObjRef{ Item.Template, Item.TemplateIndex }.Alive())
            Holder = Item.Template;                     // 身份仍在原版上
    }

    SDK::AActor* Finished = CloneItemActor(Item, Dest, Rot);
    if (!Finished)
        return nullptr;

    if (Holder)
    {
        // 诊断: Holder 的身份与 Tags 状态 (上轮 Tags 拷贝未执行, 定位源头)
        ModMenu::Log("[Spawn] Holder: [%s] Class=%p Tags=%d%s",
                     Holder->GetName().c_str(), (void*)Holder->Class, Holder->Tags.Num(),
                     Holder->Class == Finished->Class ? "" : " (类不匹配!)");
        // 身份字段深拷贝 (引擎分配器): Tags + 生成点 → 克隆体获得独立副本,
        // 原版保持完整 (曾用所有权移交掏空原版 → 游戏读空壳道具状态崩溃 + 原版无法使用)
        CopyMulticastDelegates(Holder, Finished);
        // 门钥匙引用重定向 (门 SpecialKey 指向身份持有者 → 改指新克隆)
        RedirectDoorKeys(Holder, Finished);

        // Item 引用 s_ItemClasses 内的条目 (menu 遍历 GetAllItemClasses 传入), 回写身份持有者
        const_cast<ItemClassInfo&>(Item).IdentityHolder = Finished;
        const_cast<ItemClassInfo&>(Item).HolderIndex = Finished->Index;
        ModMenu::Log("[Spawn] %s 身份字段深拷贝: Tags(%d)+生成点 → 克隆体 (原版完整保留)",
                     Item.Name.c_str(), Finished->Tags.Num());
    }
    else if (IsTaskItem)
    {
        // 已消耗 (原版与全部身份克隆均被游戏销毁): 从快照恢复交互身份,
        // 生成的仍是可用道具; 新克隆成为新身份持有者, 后续生成走正常移交路径
        if (RestoreIdentityFromSnapshot(Finished, Item))
        {
            const_cast<ItemClassInfo&>(Item).IdentityHolder = Finished;
            const_cast<ItemClassInfo&>(Item).HolderIndex = Finished->Index;
            ModMenu::Log("[Spawn] %s 快照身份恢复: 克隆为可交互道具 (新身份持有者)",
                         Item.Name.c_str());
            // 门钥匙重定向 (原版已死): 用模板悬垂指针的值匹配门的 SpecialKey
            RedirectDoorKeys(Item.Template, Finished);
        }
        else
        {
            ModMenu::Log("[Spawn] %s 快照无身份可恢复 (无 Tags), 生成外观克隆",
                         Item.Name.c_str());
        }
    }
    return Finished;
}

bool WorldToScreen(const SDK::FVector& WorldPos, SDK::FVector2D& OutScreen)
{
    if (!s_PC || !IsUsable(s_PC)) // ProjectWorldLocationToScreen 走 ProcessEvent, PC 必须可用
        return false;
    SDK::FVector2D ScreenLoc;
    if (s_PC->ProjectWorldLocationToScreen(WorldPos, &ScreenLoc, false))
    {
        OutScreen = ScreenLoc;
        return true;
    }
    return false;
}


} // namespace GameContext
