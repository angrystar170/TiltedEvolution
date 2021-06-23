#include <stdafx.h>


#include <GameServer.h>

#include <Services/EnvironmentService.h>
#include <Events/UpdateEvent.h>
#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveCellEvent.h>
#include <Messages/ServerTimeSettings.h>
#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Components.h>

EnvironmentService::EnvironmentService(World &aWorld, entt::dispatcher &aDispatcher) : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&EnvironmentService::OnUpdate>(this);
    m_joinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&EnvironmentService::OnPlayerJoin>(this);
    m_leaveCellConnection = aDispatcher.sink<PlayerLeaveCellEvent>().connect<&EnvironmentService::OnPlayerLeaveCellEvent>(this);
    m_assignObjectConnection = aDispatcher.sink<PacketEvent<AssignObjectsRequest>>().connect<&EnvironmentService::OnAssignObjectsRequest>(this);
    m_activateConnection = aDispatcher.sink<PacketEvent<ActivateRequest>>().connect<&EnvironmentService::OnActivate>(this);
    m_lockChangeConnection = aDispatcher.sink<PacketEvent<LockChangeRequest>>().connect<&EnvironmentService::OnLockChange>(this);
}

void EnvironmentService::OnPlayerJoin(const PlayerJoinEvent& acEvent) const noexcept
{
    ServerTimeSettings timeMsg;
    timeMsg.TimeScale = m_timeModel.TimeScale;
    timeMsg.Time = m_timeModel.Time;

    const auto &playerComponent = m_world.get<PlayerComponent>(acEvent.Entity);
    GameServer::Get()->Send(playerComponent.ConnectionId, timeMsg);
}

void EnvironmentService::OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent& acEvent) noexcept
{
    auto playerView = m_world.view<PlayerComponent, CellIdComponent>();

    const auto playerInCellIt = std::find_if(std::begin(playerView), std::end(playerView),
        [playerView, oldCell = acEvent.OldCell](auto entity)
    {
        const auto& cellIdComponent = playerView.get<CellIdComponent>(entity);
        return cellIdComponent.Cell == oldCell;
    });

    if (playerInCellIt != std::end(playerView))
    {
        auto objectView = m_world.view<ObjectComponent, CellIdComponent>();

        for (auto entity : objectView)
        {
            const auto& cellIdComponent = objectView.get<CellIdComponent>(entity);

            if (cellIdComponent.Cell != acEvent.OldCell)
                continue;

            m_world.destroy(entity);
        }
    }
}

void EnvironmentService::OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent>();

    AssignObjectsResponse response;

    for (const auto& object : acMessage.Packet.Objects)
    {
        const auto iter = std::find_if(std::begin(view), std::end(view), [view, id = object.Id](auto entity)
        {
            const auto formIdComponent = view.get<FormIdComponent>(entity);
            return formIdComponent.Id == id;
        });

        if (iter != std::end(view))
        {
            ObjectData objectData;

            auto& formIdComponent = view.get<FormIdComponent>(*iter);
            objectData.Id = formIdComponent.Id;

            auto& objectComponent = view.get<ObjectComponent>(*iter);
            objectData.CurrentLockData = objectComponent.CurrentLockData;

            auto& inventoryComponent = view.get<InventoryComponent>(*iter);
            objectData.CurrentInventory.Buffer = inventoryComponent.Content.Buffer;

            response.Objects.push_back(objectData);
        }
        else
        {
            const auto cEntity = m_world.create();

            m_world.emplace<FormIdComponent>(cEntity, object.Id);

            auto& objectComponent = m_world.emplace<ObjectComponent>(cEntity, acMessage.ConnectionId);
            objectComponent.CurrentLockData = object.CurrentLockData;

            m_world.emplace<CellIdComponent>(cEntity, object.CellId, object.WorldSpaceId, object.CurrentCoords);
            m_world.emplace<InventoryComponent>(cEntity);
            m_world.emplace<OwnerComponent>(cEntity, acMessage.ConnectionId);
        }
    }

    if (!response.Objects.empty())
        GameServer::Get()->Send(acMessage.ConnectionId, response);
}

void EnvironmentService::OnActivate(const PacketEvent<ActivateRequest>& acMessage) const noexcept
{
    NotifyActivate notifyActivate;
    notifyActivate.Id = acMessage.Packet.Id;
    notifyActivate.ActivatorId = acMessage.Packet.ActivatorId;

    auto view = m_world.view<PlayerComponent, CellIdComponent>();

    auto connectionId = acMessage.ConnectionId;

    auto senderIter = std::find_if(std::begin(view), std::end(view), [view, connectionId](auto entity) 
        {
            const auto& playerComponent = view.get<PlayerComponent>(entity);
            return playerComponent.ConnectionId == connectionId;
        });

    if (senderIter == std::end(view))
    {
        spdlog::warn("Player with connection id {:X} doesn't exist.", connectionId);
        return;
    }

    const auto& senderCellIdComponent = view.get<CellIdComponent>(*senderIter);

    if (senderCellIdComponent.WorldSpaceId == GameId{})
    {
        for (auto entity : view)
        {
            auto& player = view.get<PlayerComponent>(entity);
            auto& cell = view.get<CellIdComponent>(entity);

            if (player.ConnectionId != acMessage.ConnectionId && cell.Cell == acMessage.Packet.CellId)
            {
                GameServer::Get()->Send(player.ConnectionId, notifyActivate);
            }
        }
    }
    else
    {
        for (auto entity : view)
        {
            auto& player = view.get<PlayerComponent>(entity);
            auto& cell = view.get<CellIdComponent>(entity);

            if (cell.WorldSpaceId == GameId{})
                continue;

            if (player.ConnectionId != acMessage.ConnectionId 
                && cell.WorldSpaceId == senderCellIdComponent.WorldSpaceId
                && GridCellCoords::IsCellInGridCell(&cell.CenterCoords, &senderCellIdComponent.CenterCoords))
            {
                GameServer::Get()->Send(player.ConnectionId, notifyActivate);
            }
        }
    }
}

void EnvironmentService::OnLockChange(const PacketEvent<LockChangeRequest>& acMessage) const noexcept
{
    NotifyLockChange notifyLockChange;
    notifyLockChange.Id = acMessage.Packet.Id;
    notifyLockChange.IsLocked = acMessage.Packet.IsLocked;
    notifyLockChange.LockLevel = acMessage.Packet.LockLevel;

    auto objectView = m_world.view<FormIdComponent, ObjectComponent>();

    const auto iter = std::find_if(std::begin(objectView), std::end(objectView), [objectView, id = acMessage.Packet.Id](auto entity)
    {
        const auto formIdComponent = objectView.get<FormIdComponent>(entity);
        return formIdComponent.Id == id;
    });

    if (iter != std::end(objectView))
    {
        auto& objectComponent = objectView.get<ObjectComponent>(*iter);
        objectComponent.CurrentLockData.IsLocked = acMessage.Packet.IsLocked;
        objectComponent.CurrentLockData.LockLevel = acMessage.Packet.LockLevel;
    }

    auto playerView = m_world.view<PlayerComponent, CellIdComponent>();

    auto connectionId = acMessage.ConnectionId;

    auto senderIter = std::find_if(std::begin(playerView), std::end(playerView), [playerView, connectionId](auto entity) 
        {
            const auto& playerComponent = playerView.get<PlayerComponent>(entity);
            return playerComponent.ConnectionId == connectionId;
        });

    if (senderIter == std::end(playerView))
    {
        spdlog::warn("Player with connection id {:X} doesn't exist.", connectionId);
        return;
    }

    const auto& senderCellIdComponent = playerView.get<CellIdComponent>(*senderIter);

    if (senderCellIdComponent.WorldSpaceId == GameId{})
    {
        for (auto entity : playerView)
        {
            auto& player = playerView.get<PlayerComponent>(entity);
            auto& cell = playerView.get<CellIdComponent>(entity);

            if (player.ConnectionId != acMessage.ConnectionId && cell.Cell == acMessage.Packet.CellId)
            {
                GameServer::Get()->Send(player.ConnectionId, notifyLockChange);
            }
        }
    }
    else
    {
        for (auto entity : playerView)
        {
            auto& player = playerView.get<PlayerComponent>(entity);
            auto& cell = playerView.get<CellIdComponent>(entity);

            if (cell.WorldSpaceId == GameId{})
                continue;

            if (player.ConnectionId != acMessage.ConnectionId 
                && cell.WorldSpaceId == senderCellIdComponent.WorldSpaceId
                && GridCellCoords::IsCellInGridCell(&cell.CenterCoords, &senderCellIdComponent.CenterCoords))
            {
                GameServer::Get()->Send(player.ConnectionId, notifyLockChange);
            }
        }
    }
}

bool EnvironmentService::SetTime(int aHours, int aMinutes, float aScale) noexcept
{
    m_timeModel.TimeScale = aScale;

    if (aHours >= 0 && aHours <= 24 && aMinutes >= 0 && aMinutes <= 60)
    {
        // encode time as skyrim time
        auto minutes = static_cast<float>(aMinutes) * 0.17f;
        minutes = floor(minutes * 100) / 1000;
        m_timeModel.Time = static_cast<float>(aHours) + minutes;

        ServerTimeSettings timeMsg;
        timeMsg.TimeScale = m_timeModel.TimeScale;
        timeMsg.Time = m_timeModel.Time;
        GameServer::Get()->SendToLoaded(timeMsg);
        return true;
    }

    return false;
}

EnvironmentService::TTime EnvironmentService::GetTime() const noexcept
{
    const auto hour = floor(m_timeModel.Time);
    const auto minutes = (m_timeModel.Time - hour) / 17.f;

    const auto flatMinutes = static_cast<int>(ceil((minutes * 100.f) * 10.f));
    return {static_cast<int>(hour), flatMinutes};
}

EnvironmentService::TTime EnvironmentService::GetRealTime() noexcept
{
    const auto t = std::time(nullptr);
    int h = (t / 3600) % 24;
    int m = (t / 60) % 60;
    return {h, m};
}

EnvironmentService::TDate EnvironmentService::GetDate() const noexcept
{
    return {m_timeModel.Day, m_timeModel.Month, m_timeModel.Year};
}

void EnvironmentService::OnUpdate(const UpdateEvent &) noexcept
{
    if (!m_lastTick)
        m_lastTick = GameServer::Get()->GetTick();

    auto now = GameServer::Get()->GetTick();

    // client got ahead, we wait
    if (now < m_lastTick)
        return;

    auto delta = now - m_lastTick;
    m_lastTick = now;
    m_timeModel.Update(delta);
}
