#pragma once

#include "QuickMsg.h"

#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/service/Bedrock.h>
#include <mc/nbt/CompoundTag.h>
#include <mc/network/ServerNetworkHandler.h>
#include <mc/network/packet/BlockActorDataPacket.h>
#include <mc/network/packet/ContainerClosePacket.h>
#include <mc/network/packet/ContainerOpenPacket.h>
#include <mc/network/packet/InventorySlotPacket.h>
#include <mc/network/packet/ItemStackRequestPacket.h>
#include <mc/network/packet/PlaySoundPacket.h>
#include <mc/network/packet/SetLocalPlayerAsInitializedPacket.h>
#include <mc/network/packet/UpdateBlockPacket.h>
#include <mc/server/ServerLevel.h>
#include <mc/server/commands/CommandContext.h>
#include <mc/server/commands/MinecraftCommands.h>
#include <mc/server/commands/PlayerCommandOrigin.h>
#include <mc/world/Minecraft.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/events/TextProcessingEventOrigin.h>
#include <mc/world/level/BlockSource.h>
#include <mc/world/level/block/Block.h>
#include <mc/world/level/dimension/Dimension.h>
#include <memory>

ll::Logger                                                                      logger("QuickMsg");
std::unordered_map<mce::UUID, std::pair<std::string, std::pair<BlockPos, int>>> actions;
class ItemStackRequestData { // 这里需要先补个结构
public:
    TypedClientNetId<ItemStackRequestIdTag, int, 0>            mClientRequestId; // this+0x0
    std::vector<std::string>                                   mStringsToFilter;
    TextProcessingEventOrigin                                  mStringsToFilterOrigin; // this+0x28
    std::vector<std::unique_ptr<class ItemStackRequestAction>> mActions;
};

namespace QuickMsg {

static std::unique_ptr<Entry> instance;

Entry& Entry::getInstance() { return *instance; }

bool Entry::load() { return true; }

bool Entry::enable() { return true; }

bool Entry::disable() { return true; }

bool Entry::unload() { return true; }

} // namespace QuickMsg

LL_REGISTER_MOD(QuickMsg::Entry, QuickMsg::instance);

// 玩家交互实体Hook
LL_AUTO_TYPE_INSTANCE_HOOK(
    PlayerInteractActorHook,
    HookPriority::Normal,
    Player,
    &Player::interact,
    bool,
    Actor&      actor,
    Vec3 const& location
) {
    auto result = origin(actor, location);
    if (!result && actor.isType(ActorType::Player) && isSneaking() && !actions.contains(getUuid())) {
        // 铁砧方块坐标
        auto pos = getFeetBlockPos().add({0, 3, 0});

        // 更新方块类型(用于改变方块类型)
        UpdateBlockPacket(
            pos,
            static_cast<uint>(UpdateBlockPacket::BlockLayer::Standard),
            Block::tryGetFromRegistry("minecraft:anvil")->getRuntimeId(),
            static_cast<uchar>(BlockUpdateFlag::All)
        )
            .sendTo(*this);

        // 构建并发送打开容器数据包，帮玩家打开铁砧
        ContainerOpenPacket(
            static_cast<ContainerID>(-65), // 这个地方比较特殊，这是会话ID，不是类型ID，写死就行
            ContainerType::Anvil,
            pos,
            ActorUniqueID(-1) // 如果要打开实体的容器就要写上实体的UniqueID，如果是方块，一定要写-1 一定要写-1！！！
        )
            .sendTo(*this);


        // 构建并发送物品更新数据包，使铁砧第一格为指定的物品
        InventorySlotPacket(
            ContainerID::PlayerUIOnly, // 直接写UI栏就行了，物品是存储在UI栏下的（铁砧没有真实的容器）
            static_cast<int>(PlayerUISlot::AnvilInput),
            ItemStack::fromTag(
                CompoundTag::fromSnbt("{\"Count\":1b,\"Name\":\"minecraft:paper\",\"tag\":{\"display\":{\"Lore\":["
                                      "\"§r§6请在上面输入框输入您想说的话§r\"],\"Name\":\"\"},\"ench\":[]}}")
                    .value()
            )
        )
            .sendTo(*this);

        // 记录玩家信息缓存
        actions.emplace(
            getUuid(),
            std::pair(static_cast<Player&>(actor).getRealName(), std::pair(pos, getDimensionId()))
        );
    }
    return result;
}

// 物品请求包Hook
LL_AUTO_TYPE_INSTANCE_HOOK(
    SendItemStackRequestPacketHook,
    HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::handle,
    void,
    NetworkIdentifier const&      source,
    ItemStackRequestPacket const& packet
) {
    auto player = getServerPlayer(source, packet.mClientSubId);
    if (!player.has_value() || !actions.contains(player->getUuid()) || packet.mRequestBatch->getRequests().size() == 0
        || packet.mRequestBatch->getRequests().at(0)->mStringsToFilter.size() == 0
        || packet.mRequestBatch->getRequests().at(0)->mStringsToFilter.at(0) == "")
        return origin(source, packet);
    // 帮助玩家输入指令(别问我为什么不调用函数，我懒得研究了(()
    auto context = CommandContext(
        fmt::format(
            "tell \"{}\" {}",
            actions.at(player->getUuid()).first,
            packet.mRequestBatch->getRequests().at(0)->mStringsToFilter.at(0)
        ),
        std::make_unique<PlayerCommandOrigin>(PlayerCommandOrigin(*player))
    );
    ll::service::getMinecraft()->getCommands().executeCommand(context);
    PlaySoundPacket("random.orb", player->getPosition(), 1.0f, 1.2f).sendTo(player); // 播放声音
    return origin(source, packet); // 让Mojang的屎山代码帮我们归位OvO
}

// 处理关闭容器数据包Hook
LL_AUTO_TYPE_INSTANCE_HOOK(
    ContainerClosePacketSendHook,
    HookPriority::Low,
    ServerNetworkHandler,
    "?handle@ServerNetworkHandler@@UEAAXAEBVNetworkIdentifier@@AEBVContainerClosePacket@@@Z",
    void,
    NetworkIdentifier&    identifier,
    ContainerClosePacket& packet
) {
    origin(identifier, packet);
    auto player = getServerPlayer(identifier, packet.mClientSubId);
    if (player.has_value() && actions.contains(player->getUuid())) {
        try {
            // 恢复方块
            // 这里先获取一下要恢复的方块
            auto& block = ll::service::getLevel()
                              ->getDimension(actions.at(player->getUuid()).second.second)
                              ->getBlockSourceFromMainChunkSource()
                              .getBlock(actions.at(player->getUuid()).second.first);

            // 先更新方块类型
            UpdateBlockPacket(
                actions.at(player->getUuid()).second.first,
                static_cast<uint>(UpdateBlockPacket::BlockLayer::Standard),
                block.getRuntimeId(),
                static_cast<uchar>(BlockUpdateFlag::All)
            )
                .sendTo(*player);

            // 再更新方块实体数据(如头颅什么的，虽然可以不写，但建议写一下)
            BlockActorDataPacket(actions.at(player->getUuid()).second.first, block.getSerializationId())
                .sendTo(*player);
        } catch (...) {}
        actions.erase(player->getUuid());
    }
}

// 玩家进服Hook
LL_AUTO_TYPE_INSTANCE_HOOK(
    PlayerJoinHook,
    HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::handle,
    void,
    NetworkIdentifier const&                 identifier,
    SetLocalPlayerAsInitializedPacket const& packet
) {
    origin(identifier, packet);
    // 没啥好说的，就清除缓存
    auto player = getServerPlayer(identifier, packet.mClientSubId);
    if (player.has_value() && actions.contains(player->getUuid())) actions.erase(player->getUuid());
}