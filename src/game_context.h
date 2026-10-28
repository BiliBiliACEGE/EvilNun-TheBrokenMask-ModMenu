#pragma once
// 游戏上下文: 获取并缓存 玩家/修女/PlayerController/GameInstance

#include "common.h"

#include <SDK.hpp>

namespace GameContext {

// 每帧调用, 刷新缓存 (指针可能因关卡切换失效)
void Update();

// 对象可用性检查: 有效 且 未被引擎标记销毁
// (关卡切换时 Destroy 后、GC 移除前的"半销毁"对象仍注册在 GObjects,
//  IsValidUObject 会误判为真; 对其写属性/调函数会崩溃 0x8)
bool IsUsable(SDK::UObject* Obj);

SDK::UWorld* GetWorld();
SDK::APlayerController* GetPlayerController();
SDK::AEvilNun_Player_Character_C* GetPlayer();
SDK::UEvilNunGameInstance_C* GetGameInstance();

// 获取所有修女 (Madelaine / 胖修女 Philippa 等), 内部缓存, 每秒刷新一次
const std::vector<SDK::ACharacter*>& GetNuns();

// 获取所有可拾取道具 (ANunGrabObject_C: 钥匙/炸药/火把/圣杯等), 内部缓存, 每秒刷新一次
const std::vector<SDK::ANunGrabObject_C*>& GetGrabObjects();

// 获取所有物理杂物 (APhysGrabObject_C: 盘子/瓶子/书本等), 内部缓存, 每秒刷新一次
const std::vector<SDK::APhysGrabObject_C*>& GetPhysObjects();

// 物品配置快照 (类型化字段, SDK 成员直接访问, 偏移由 Dumper-7 断言保证;
// 生成时应用, 不依赖模板实例存活)
struct ItemConfigSnapshot
{
    // 通用
    SDK::FVector TemplateScale;
    SDK::FVector Scale;
    bool CollideWithPlayer = false;
    SDK::FName ActorName;                            // 模板 Actor 名 (场景交互按名识别, 如 "Fuse_2")
    std::vector<SDK::FName> Tags;                    // Actor 标签 (蓝图交互检查项)
    // NunGrabObject (任务道具)
    SDK::FEvilNunInteractStructure GrabConfig;
    SDK::UStaticMesh* Mesh = nullptr;              // 显示网格 (Phys 为 PhysicsMesh)
    SDK::UMaterialInterface* Material = nullptr;
    double GrabbedScale = 1.0;
    SDK::FVector GrabbedLocationOffset;
    SDK::FRotator GrabbedRotationOffset;
    double DropDistance = 200.0;
    double DropSpaceScale = 1.0;
    double DropImpulseScale = 1.0;
    SDK::USoundBase* GrabSound = nullptr;
    bool IsBunkerKey = false;
    bool IsDynamite = false;
    uint8_t ObjectKind = 0;
    bool BeginsAwake = false;
    bool TrueChargedDoll = false;
    SDK::AActor* NestingActor = nullptr;           // 放入目标容器 (保险丝盒等; 交互判定的核心凭据)
    int32_t NestingActorIndex = -1;                // 上者的 GObjects 索引 (无解引用存活校验)
    SDK::FVector ReespawnLocation;                 // 重生位置 (丢弃/放置回落点)
    std::vector<SDK::AActor*> PossibleSpawnLocations; // 可选生成点
    std::vector<int32_t> PossibleSpawnLocationIndexes; // 与上面一一对应的 GObjects 索引
    // 实例级属性通用快照 (反射捕获): 手挑字段清单覆盖不了逐实例配置 —
    // 如小雕像的 BreakPieces 碎片网格 (关卡里逐实例配置, CDO 默认为空 →
    // 克隆摔不碎)。原始属性按字节回放, 对象数组走引擎分配器通道回放
    struct PrimSnap { uint16_t Offset; uint8_t Size; uint8_t Mask; uint8_t Data[8]; };
    std::vector<PrimSnap> Prims;
    struct ObjArraySnap
    {
        std::string PropName;                      // 属性名 (回放时按名定位)
        std::vector<SDK::UObject*> Ptrs;           // 元素指针
        std::vector<int32_t> Indexes;              // 对应 GObjects 索引 (存活校验)
    };
    std::vector<ObjArraySnap> ObjArrays;
    // PhysGrabObject (物理杂物)
    SDK::FInteractConfigStruct PhysConfig;
    SDK::FRotator GrabRotation;
    bool OrientObjectToCamera = false;
    bool StartAwaken = false;
    bool EnableGravity = true;
    bool FloatsInWater = false;
    double MassScale = 1.0;
    double MaxHoldDistance = 300.0;
    double MaxVelocity = 0.0;
};

// 物品模板信息 (按 Mesh+功能 去重, 名称参与优先级合并, 覆盖所有已加载关卡中的物品)
struct ItemClassInfo
{
    SDK::UClass* Cls = nullptr;   // 物品类 (NunGrabObject_C / PhysGrabObject_C)
    std::string Name;             // 显示名 (游戏内交互文本 / MyselfName, 空则 Mesh 名)
    std::string MeshName;         // 网格资源名 (搜索备用匹配)
    std::string DedupKey;         // 去重键 (Mesh+功能)
    const char* Group = "";       // 分组: 任务道具 / 物理杂物
    ItemConfigSnapshot Snap;      // 配置快照
    SDK::AActor* Template = nullptr;  // 关卡原版实例 (任务道具交互凭据)
    int32_t TemplateIndex = -1;       // 模板的 GObjects 索引 (无解引用存活校验)
    bool Consumed = false;            // 原版已被消耗 (仅剩克隆), 只能克隆外观
    SDK::AActor* IdentityHolder = nullptr; // 当前持有交互身份的实例 (生成时身份转移到最新克隆, null=身份在原版上)
    int32_t HolderIndex = -1;         // 身份持有者的 GObjects 索引 (无解引用存活校验)
};

// 遍历 GObjects 收集所有已加载物品实例并去重 (含流式关卡), 定期增量合并
const std::vector<ItemClassInfo>& GetAllItemClasses();
void RescanItemClasses(); // 强制重新扫描

// 实例 → 物品显示名 (物品扫描构建: 模板/普通实例/克隆均覆盖; 克隆在生成时记录)
// 无映射返回 nullptr (未扫描/不可识别实例) — ESP 高亮等显示用
const char* GetInstanceDisplayName(SDK::UObject* O);

// 以配置快照为基准, 在玩家面前生成一个新物品 (不依赖模板实例存活)
SDK::AActor* SpawnObjectFromTemplate(const ItemClassInfo& Item, double Distance);

// 任务道具是否仍可生成"可交互"道具 (身份持有者存活, 或原版仍存活未转移过身份)
bool IsTemplateAlive(const ItemClassInfo& Item);

// 世界坐标 -> 屏幕坐标 (用于 ESP), 失败返回 false
bool WorldToScreen(const SDK::FVector& WorldPos, SDK::FVector2D& OutScreen);

// 全 GObjects 清扫 "async+PendingKill" 僵尸对象 (拖住旧 World 的元凶)。
// 公开给 dllmain 的 GC 校验器 hook: 校验器运行前先清扫, 零时间盲区
void SweepZombieAsyncFlags(const char* When);

// 清除 World/Level 自身的 async 内部位 (async=GC 推迟销毁标志, 带位的旧 World 即使无任何引用也不会被 GC 销毁 → "not cleaned up by GC / flag set" Fatal)。
// IncludeCurrent=true: 连当前活跃 World 及其 PersistentLevel 一起清 — 转场 GC 在世界
// 指针切换【之前】执行, 旧 World 此时仍是"当前"World, 不连它一起清必然带位过转场
// (2026-08-28 Fatal 实测)。当前 World 处于引擎根集保护下, 清位零风险。
// 两个调用点: Update 周期清扫 (游玩态常态化拆除引信, 与转场时机无关) +
// dllmain GC 校验器 hook 入口 (兜底)。
void ClearOldWorldAsyncFlags(bool IncludeCurrent = false);

// 强制加载当前 World 的所有流式子关卡 (补全道具列表用):
// 道具实例只存在于已加载的子关卡中, 远处子关卡未加载时扫描不到其道具。
// 返回触发加载的子关卡数 (-1=World 无效)
int ForceLoadStreamingLevels();

// 把所有 Grab/Phys 实例原始信息写入日志 (类名/Actor名/Mesh/文本/Tags),
// 用于定位"列表缺失物品"(如漆桶)的真实身份与被跳过原因
void DumpAllItemInstances();

// ============ 传送点 / 关卡 ============
struct TeleportPoint
{
    std::string Name;    // 房间名 (子关卡) 或 PlayerStart 的 Actor 名
    SDK::FVector Pos;
    bool IsRoom = false; // true=流式子关卡 (房间/场景), false=出生点
    SDK::ULevelStreaming* SrcLevel = nullptr; // IsRoom 时的流式子关卡 (传送前触发加载)
};
// 收集当前关卡所有 PlayerStart 作为传送点 (内部缓存, 5 秒刷新)
const std::vector<TeleportPoint>& GetTeleportPoints();

} // namespace GameContext
