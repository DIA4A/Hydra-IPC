/*
 * Hydra IPC Library
 * Copyright (c) 2026 DIA4A
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this project.
 */

#pragma once
#include "HydraProtocol.h"
#include <functional>
#include <unordered_map>
#include <type_traits>

namespace HydraIPC
{
	namespace Peer
	{
		// Raw handler: receives sender slot, raw payload pointer, and payload size.
		using RawHandler = std::function<void(uint32_t uSenderSlot, const void* pPayload, uint32_t uPayloadSize)>;

		// Default handler: also receives the command type for dispatch.
		using DefaultHandler = std::function<void(uint32_t uSenderSlot, uint32_t uCommandType, const void* pPayload, uint32_t uPayloadSize)>;

		inline HANDLE              m_hFileMapping = NULL;
		inline SharedMemoryLayout* m_pShared = nullptr;
		inline int32_t             m_nMySlot = -1;
		inline volatile bool       m_bAPCRunning = false;
		inline HANDLE              m_hAPCThread = NULL;
		inline uint32_t            m_nReadIndex = 0;
		inline bool                m_bCreatedSharedMemory = false;
		inline HANDLE              m_hPeerThreads[MAX_PEERS] = {};

		// NOTE: Register all handlers BEFORE calling Join() or while the APC thread is paused. The APC thread reads this map without locking.
		inline std::unordered_map<uint32_t, RawHandler> m_handlers;
		inline DefaultHandler m_defaultHandler;

		inline PeerSlot* GetMySlot()
		{
			if (!m_pShared || m_nMySlot < 0)
			{
				return nullptr;
			}
			return &m_pShared->peerSlots[m_nMySlot];
		}

		inline void ProcessPendingCommands()
		{
			if (!m_pShared)
			{
				return;
			}

			auto& ring = m_pShared->commandRing;

			while (true)
			{
				uint32_t uSlot = m_nReadIndex & COMMAND_RING_MASK;
				CommandEntry& entry = ring.entries[uSlot];

				uint32_t uExpectedSeq = m_nReadIndex + 1;
				if (entry.sequence != uExpectedSeq)
				{
					break;
				}

				MemoryBarrier();

				if (entry.targetMask & TargetSlot(m_nMySlot))
				{
					auto it = m_handlers.find(entry.commandType);
					if (it != m_handlers.end())
					{
						it->second(entry.senderSlot, entry.payload, entry.payloadSize);
					}
					else if (m_defaultHandler)
					{
						m_defaultHandler(entry.senderSlot, entry.commandType, entry.payload, entry.payloadSize);
					}
				}

				m_nReadIndex++;
			}
		}

		inline DWORD WINAPI APCThreadProc(LPVOID)
		{
			while (m_bAPCRunning)
			{
				SleepEx(100, TRUE);

				if (!m_bAPCRunning)
				{
					break;
				}

				ProcessPendingCommands();
			}
			return 0;
		}

		inline void SignalTargets(PeerMask uTargetMask)
		{
			if (!m_pShared)
			{
				return;
			}

			for (int i = 0; i < (int)MAX_PEERS; i++)
			{
				if (i == m_nMySlot)
				{
					continue;
				}

				if (!(uTargetMask & (PeerMask(1) << i)))
				{
					continue;
				}

				PeerSlot& slot = m_pShared->peerSlots[i];
				if (slot.connectState != 1 || !slot.apcCallbackAddr)
				{
					continue;
				}

				if (!m_hPeerThreads[i])
				{
					m_hPeerThreads[i] = OpenThread(THREAD_SET_CONTEXT, FALSE, slot.apcThreadId);
					if (!m_hPeerThreads[i])
					{
						continue;
					}
				}

				if (!QueueUserAPC((PAPCFUNC)slot.apcCallbackAddr, m_hPeerThreads[i], 0))
				{
					CloseHandle(m_hPeerThreads[i]);
					m_hPeerThreads[i] = NULL;
				}
			}
		}

#pragma region Core Lifecycle

		inline bool IsJoined()
		{
			return m_pShared != nullptr && m_nMySlot >= 0;
		}

		inline bool IsHydraActive()
		{
			return m_pShared && m_pShared->isActive;
		}

		// Join the hive. First peer creates the shared memorym subsequent peers attach to the existing mapping. Returns true on success.
		inline bool Join(const char* szPeerName, uint32_t uProcessId)
		{
			if (m_pShared)
			{
				return false;
			}

			m_hFileMapping = CreateFileMappingA(
				INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
				0, sizeof(SharedMemoryLayout), SharedMemoryName()
			);
			if (!m_hFileMapping)
			{
				return false;
			}

			m_bCreatedSharedMemory = (GetLastError() != ERROR_ALREADY_EXISTS);

			m_pShared = (SharedMemoryLayout*)MapViewOfFile(
				m_hFileMapping, FILE_MAP_ALL_ACCESS,
				0, 0, sizeof(SharedMemoryLayout)
			);
			if (!m_pShared)
			{
				CloseHandle(m_hFileMapping);
				m_hFileMapping = NULL;
				return false;
			}

			if (m_bCreatedSharedMemory)
			{
				memset(m_pShared, 0, sizeof(SharedMemoryLayout));
				m_pShared->magic = SHARED_MEMORY_MAGIC;
				m_pShared->version = SHARED_MEMORY_VERSION;
				InterlockedExchange(&m_pShared->leaderSlot, -1);
				InterlockedExchange(&m_pShared->isActive, 1);
			}
			else
			{
				if (m_pShared->magic != SHARED_MEMORY_MAGIC ||
					m_pShared->version != SHARED_MEMORY_VERSION)
				{
					UnmapViewOfFile(m_pShared);
					m_pShared = nullptr;
					CloseHandle(m_hFileMapping);
					m_hFileMapping = NULL;
					return false;
				}
			}

			m_bAPCRunning = true;
			m_hAPCThread = CreateThread(NULL, 0, APCThreadProc, NULL, 0, NULL);
			if (!m_hAPCThread)
			{
				m_bAPCRunning = false;
				UnmapViewOfFile(m_pShared);
				m_pShared = nullptr;
				CloseHandle(m_hFileMapping);
				m_hFileMapping = NULL;
				return false;
			}

			m_nReadIndex = (uint32_t)m_pShared->commandRing.writeIndex;

			m_nMySlot = -1;
			for (int i = 0; i < (int)MAX_PEERS; i++)
			{
				if (InterlockedCompareExchange(&m_pShared->peerSlots[i].connectState, 1, 0) == 0)
				{
					m_nMySlot = i;
					break;
				}
			}

			if (m_nMySlot < 0)
			{
				m_bAPCRunning = false;
				QueueUserAPC((PAPCFUNC)&APCCallback, m_hAPCThread, 0);
				WaitForSingleObject(m_hAPCThread, 1000);
				CloseHandle(m_hAPCThread);
				m_hAPCThread = NULL;
				UnmapViewOfFile(m_pShared);
				m_pShared = nullptr;
				CloseHandle(m_hFileMapping);
				m_hFileMapping = NULL;
				return false;
			}

			PeerSlot& slot = m_pShared->peerSlots[m_nMySlot];
			slot.processId = uProcessId;
			slot.apcThreadId = GetThreadId(m_hAPCThread);
			slot.apcCallbackAddr = (uintptr_t)&APCCallback;
			slot.speed = 0.f;
			slot.objectId = -1;
			slot.worldId = 0;
			strncpy_s(slot.name, szPeerName, sizeof(slot.name) - 1);
			InterlockedExchange(&slot.isSlowed, 0);
			InterlockedExchange(&slot.isConnectedInGame, 0);

			if (m_bCreatedSharedMemory)
			{
				InterlockedExchange(&m_pShared->leaderSlot, m_nMySlot);
			}

			return true;
		}

		inline void Leave()
		{
			if (!m_pShared)
			{
				return;
			}

			InterlockedCompareExchange(&m_pShared->leaderSlot, -1, m_nMySlot);

			if (m_nMySlot >= 0)
			{
				InterlockedExchange(&m_pShared->peerSlots[m_nMySlot].connectState, 0);
				m_nMySlot = -1;
			}

			m_bAPCRunning = false;
			if (m_hAPCThread)
			{
				QueueUserAPC((PAPCFUNC)&APCCallback, m_hAPCThread, 0);
				WaitForSingleObject(m_hAPCThread, 2000);
				CloseHandle(m_hAPCThread);
				m_hAPCThread = NULL;
			}

			for (int i = 0; i < (int)MAX_PEERS; i++)
			{
				if (m_hPeerThreads[i])
				{
					CloseHandle(m_hPeerThreads[i]);
					m_hPeerThreads[i] = NULL;
				}
			}

			m_handlers.clear();
			m_defaultHandler = nullptr;

			UnmapViewOfFile(m_pShared);
			m_pShared = nullptr;
			CloseHandle(m_hFileMapping);
			m_hFileMapping = NULL;
		}

#pragma endregion

#pragma region Leadership

		inline int32_t GetLeaderSlot()
		{
			if (!m_pShared)
			{
				return -1;
			}
			return m_pShared->leaderSlot;
		}

		inline bool IsLeader()
		{
			return m_pShared && m_nMySlot >= 0 && m_pShared->leaderSlot == m_nMySlot;
		}

		// Atomically claim leadership. Returns true if we are now the leader.
		inline bool ClaimLeadership()
		{
			if (!m_pShared || m_nMySlot < 0)
			{
				return false;
			}

			if (InterlockedCompareExchange(&m_pShared->leaderSlot, m_nMySlot, -1) == -1)
			{
				return true;
			}

			InterlockedExchange(&m_pShared->leaderSlot, m_nMySlot);
			return true;
		}

		inline void ReleaseLeadership()
		{
			if (m_pShared)
			{
				InterlockedCompareExchange(&m_pShared->leaderSlot, -1, m_nMySlot);
			}
		}

		// Checks if the current leader's process is still alive. If dead, purges the leader's slot and self promotes.
		inline void WatchdogCheckLeader()
		{
			if (!m_pShared)
			{
				return;
			}

			int32_t nLeader = m_pShared->leaderSlot;
			if (nLeader < 0 || nLeader >= (int32_t)MAX_PEERS || nLeader == m_nMySlot)
			{
				return;
			}

			PeerSlot& slot = m_pShared->peerSlots[nLeader];
			if (slot.connectState != 1)
			{
				ClaimLeadership();
				return;
			}

			HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, slot.processId);
			if (!hProc)
			{
				InterlockedExchange(&slot.connectState, 0);
				ClaimLeadership();
				return;
			}

			DWORD dwResult = WaitForSingleObject(hProc, 0);
			CloseHandle(hProc);

			if (dwResult != WAIT_TIMEOUT)
			{
				InterlockedExchange(&slot.connectState, 0);
				ClaimLeadership();
			}
		}

#pragma endregion

#pragma region Leader Broadcast State

		inline void BeginStateUpdate()
		{
			if (m_pShared)
			{
				Seqlock::BeginWrite(m_pShared->leaderState.sequence);
			}
		}

		inline void EndStateUpdate()
		{
			if (m_pShared)
			{
				Seqlock::EndWrite(m_pShared->leaderState.sequence);
			}
		}

		// Returns a writable reference. Must be called between Begin/EndStateUpdate.
		inline LeaderBroadcastState& State()
		{
			return m_pShared->leaderState;
		}

		// Reads a consistent snapshot of the leader's broadcast state.
		inline bool PollState(LeaderBroadcastState& outState)
		{
			if (!m_pShared)
			{
				return false;
			}

			const auto& state = m_pShared->leaderState;
			int nRetries = 0;

			long nSeq;
			do
			{
				nSeq = Seqlock::BeginRead(state.sequence);
				memcpy(&outState, (const void*)&state, sizeof(LeaderBroadcastState));
			} while (!Seqlock::ValidateRead(state.sequence, nSeq) && ++nRetries < 100);

			return nRetries < 100;
		}

		inline void UpdatePosition(float fX, float fY, bool bNoClip, bool bForceWalk)
		{
			if (!m_pShared)
			{
				return;
			}

			auto& s = m_pShared->leaderState;
			Seqlock::BeginWrite(s.sequence);
			{
				s.posX = fX;
				s.posY = fY;
			}
			Seqlock::EndWrite(s.sequence);
		}

		inline void UpdateShootState(bool bShouldShoot, float fX, float fY)
		{
			if (!m_pShared)
			{
				return;
			}

			auto& s = m_pShared->leaderState;
			Seqlock::BeginWrite(s.sequence);
			{
				s.shouldShoot = bShouldShoot;
				s.shootX = fX;
				s.shootY = fY;
			}
			Seqlock::EndWrite(s.sequence);
		}

		inline void UpdateLeaderInfo(uint32_t uWorldId, int32_t nObjectId, const char* szName, const char* szIP, bool bAllowCrossWorld)
		{
			if (!m_pShared)
			{
				return;
			}

			auto& s = m_pShared->leaderState;
			Seqlock::BeginWrite(s.sequence);
			{
				s.leaderWorldId = uWorldId;
				s.leaderObjectId = nObjectId;
				if (szName)
				{
					strncpy_s(s.leaderName, szName, sizeof(s.leaderName) - 1);
				}
				if (szIP)
				{
					strncpy_s(s.leaderServerAddress, szIP, sizeof(s.leaderServerAddress) - 1);
				}
				s.allowCrossWorldConnections = bAllowCrossWorld;
			}
			Seqlock::EndWrite(s.sequence);
		}

#pragma endregion

#pragma region Command Sending

		// Push a raw command into the ring buffer and signal targets.
		inline void PushCommand(uint32_t uCommandType, const void* pPayload, uint32_t uPayloadSize, PeerMask uTargetMask = TargetAll)
		{
			if (!m_pShared || m_nMySlot < 0)
			{
				return;
			}

			if (uPayloadSize > MAX_PAYLOAD_SIZE)
			{
				return;
			}

			uTargetMask &= ~TargetSlot(m_nMySlot);

			auto& ring = m_pShared->commandRing;
			long nIndex = InterlockedIncrement(&ring.writeIndex) - 1;
			uint32_t uSlot = (uint32_t)nIndex & COMMAND_RING_MASK;

			CommandEntry& entry = ring.entries[uSlot];
			entry.commandType = uCommandType;
			entry.senderSlot = (uint32_t)m_nMySlot;
			entry.targetMask = uTargetMask;
			entry.payloadSize = uPayloadSize;
			if (pPayload && uPayloadSize > 0)
			{
				memcpy(entry.payload, pPayload, uPayloadSize);
			}
			MemoryBarrier();
			entry.sequence = (uint32_t)nIndex + 1;

			SignalTargets(uTargetMask);
		}

		template <typename T>
		inline void Send(uint32_t uCommandType, const T& data, PeerMask uTargetMask = TargetAll)
		{
			static_assert(std::is_trivially_copyable_v<T>, "Command payload must be trivially copyable (POD)");
			static_assert(sizeof(T) <= MAX_PAYLOAD_SIZE, "Command payload exceeds MAX_PAYLOAD_SIZE");
			PushCommand(uCommandType, &data, sizeof(T), uTargetMask);
		}

		// Send with no payload (for commands like CmdNexus, CmdForceNexus).
		inline void Send(uint32_t uCommandType, PeerMask uTargetMask = TargetAll)
		{
			PushCommand(uCommandType, nullptr, 0, uTargetMask);
		}

#pragma region Handler Registration

		// NOTE: Register all handlers BEFORE calling Join(), or ensure the APC thread is not actively processing commands when modifying handlers.
		// The APC thread reads the handler map without locking for performance.

		// Register a raw handler for a specific command type.
		inline void RegisterHandler(uint32_t uCommandType, RawHandler handler)
		{
			m_handlers[uCommandType] = std::move(handler);
		}

		// Usage: On<Cmd::UsePortal>(CmdUsePortal, [](uint32_t uSender, const Cmd::UsePortal& d) { ... });
		template <typename T>
		inline void On(uint32_t uCommandType, std::function<void(uint32_t uSenderSlot, const T& data)> handler)
		{
			static_assert(std::is_trivially_copyable_v<T>, "Handler payload type must be trivially copyable");
			m_handlers[uCommandType] = [handler = std::move(handler)](uint32_t uSender, const void* pPayload, uint32_t uSize)
				{
					handler(uSender, *reinterpret_cast<const T*>(pPayload));
				};
		}

		// Catch all handler for command types without a registered handler
		inline void SetDefaultHandler(DefaultHandler handler)
		{
			m_defaultHandler = std::move(handler);
		}

		inline void ClearHandler(uint32_t uCommandType)
		{
			m_handlers.erase(uCommandType);
		}

		inline void ClearAllHandlers()
		{
			m_handlers.clear();
			m_defaultHandler = nullptr;
		}

#pragma endregion

#pragma region Command Wrappers

		inline void Nexus(PeerMask uTargetMask = TargetAll)
		{
			Send(CmdNexus, uTargetMask);
		}

		inline void UsePortal(int32_t nObjectId, PeerMask uTargetMask = TargetAll)
		{
			Cmd::UsePortal cmdUsePortal;
			cmdUsePortal.nObjectId = nObjectId;
			Send(CmdUsePortal, cmdUsePortal, uTargetMask);
		}

		inline void CurrentWorld(uint32_t uWorldId, PeerMask uTargetMask = TargetAll)
		{
			Cmd::CurrentWorld cmdCurrentWorld;
			cmdCurrentWorld.uWorldId = uWorldId;
			Send(CmdCurrentWorld, cmdCurrentWorld, uTargetMask);
		}

		inline void ForceNexus(PeerMask uTargetMask = TargetAll)
		{
			Send(CmdForceNexus, uTargetMask);
		}

		inline void IngameDisconnect(PeerMask uTargetMask = TargetAll)
		{
			Send(CmdIngameDisconnect, uTargetMask);
		}

		inline void ChangeServer(int32_t nServerIndex, PeerMask uTargetMask = TargetAll)
		{
			Cmd::ChangeServer cmdChangeServer;
			cmdChangeServer.nServerIndex = nServerIndex;
			Send(CmdChangeServer, cmdChangeServer, uTargetMask);
		}

		inline void ServerIPConnect(const char* szIP, PeerMask uTargetMask = TargetAll)
		{
			Cmd::ServerIPConnect cmdServerIPConnect;
			strncpy_s(cmdServerIPConnect.szServerIP, szIP, sizeof(cmdServerIPConnect.szServerIP) - 1);
			Send(CmdServerIPConnect, cmdServerIPConnect, uTargetMask);
		}

		inline void SetUnplug(bool bEnable, PeerMask uTargetMask = TargetAll)
		{
			Cmd::Unplug cmdUnplug;
			cmdUnplug.bEnable = bEnable;
			Send(CmdUnplug, cmdUnplug, uTargetMask);
		}

		inline void LoadConfig(int32_t nConfigIndex, PeerMask uTargetMask = TargetAll)
		{
			Cmd::LoadConfig cmdLoadConfig;
			cmdLoadConfig.nConfigIndex = nConfigIndex;
			Send(CmdLoadConfig, cmdLoadConfig, uTargetMask);
		}

		inline void RunCommand(const char* szCommand, PeerMask uTargetMask = TargetAll)
		{
			Cmd::RunCommand cmdRunCommand;
			strncpy_s(cmdRunCommand.szCommand, szCommand, sizeof(cmdRunCommand.szCommand) - 1);
			Send(CmdRunCommand, cmdRunCommand, uTargetMask);
		}

		inline void ChatMessage(const char* szMessage, PeerMask uTargetMask = TargetAll)
		{
			Cmd::ChatMessage cmdChatMessage;
			strncpy_s(cmdChatMessage.szMessage, szMessage, sizeof(cmdChatMessage.szMessage) - 1);
			Send(CmdChatMessage, cmdChatMessage, uTargetMask);
		}

		inline void FollowTarget(int32_t nObjectId, PeerMask uTargetMask = TargetAll)
		{
			Cmd::FollowTarget cmdFollowTarget;
			cmdFollowTarget.nObjectId = nObjectId;
			Send(CmdFollowTarget, cmdFollowTarget, uTargetMask);
		}

		inline void RequestTome(PeerMask uTargetMask = TargetAll)
		{
			Send(CmdRequestTome, uTargetMask);
		}

		inline void Swapout(int32_t nSwapToObjectType, PeerMask uTargetMask = TargetAll)
		{
			Cmd::Swapout cmdSwapout;
			cmdSwapout.nSwapToObjectType = nSwapToObjectType;
			Send(CmdSwapout, cmdSwapout, uTargetMask);
		}

		inline void UseAbility(float flX, float flY, PeerMask uTargetMask = TargetAll)
		{
			Cmd::UseAbility cmdUseAbility;
			cmdUseAbility.flX = flX;
			cmdUseAbility.flY = flY;
			Send(CmdUseAbility, cmdUseAbility, uTargetMask);
		}

		inline void SendObjectId(int32_t nObjectId, PeerMask uTargetMask = TargetAll)
		{
			Cmd::InformObjectId cmdInformObjectId;
			cmdInformObjectId.nObjectId = nObjectId;
			Send(CmdInformObjectId, cmdInformObjectId, uTargetMask);
		}

#pragma endregion

#pragma region Peer State Writing

		inline void UpdateSpeed(float fSpeed)
		{
			PeerSlot* pSlot = GetMySlot();
			if (pSlot)
			{
				pSlot->speed = fSpeed;
			}
		}

		inline void UpdateObjectId(int32_t nObjectId)
		{
			PeerSlot* pSlot = GetMySlot();
			if (pSlot)
			{
				pSlot->objectId = nObjectId;
			}
		}

		inline void UpdateWorldId(uint32_t uWorldId)
		{
			PeerSlot* pSlot = GetMySlot();
			if (pSlot)
			{
				pSlot->worldId = uWorldId;
			}
		}

		inline void UpdateSlowed(bool bIsSlowed)
		{
			PeerSlot* pSlot = GetMySlot();
			if (pSlot)
			{
				InterlockedExchange(&pSlot->isSlowed, bIsSlowed ? 1 : 0);
			}
		}

		inline void UpdateConnectedInGame(bool bConnected)
		{
			PeerSlot* pSlot = GetMySlot();
			if (pSlot)
			{
				InterlockedExchange(&pSlot->isConnectedInGame, bConnected ? 1 : 0);
			}
		}

		inline void UpdateInventory(const int32_t* pInventory, int32_t nCount, int32_t nBackpackSlots)
		{
			PeerSlot* pSlot = GetMySlot();
			if (!pSlot)
			{
				return;
			}
			int32_t nCopyCount = nCount < 28 ? nCount : 28;
			memcpy(pSlot->inventory, pInventory, nCopyCount * sizeof(int32_t));
			pSlot->backpackSlots = nBackpackSlots;
		}

#pragma endregion

#pragma region Peer Iteration & Aggregation

		inline int32_t GetMySlotIndex()
		{
			return m_nMySlot;
		}

		inline int32_t RefreshPeerCount()
		{
			if (!m_pShared)
			{
				return 0;
			}

			int32_t nCount = 0;
			for (int i = 0; i < (int)MAX_PEERS; i++)
			{
				if (m_pShared->peerSlots[i].connectState == 1)
				{
					nCount++;
				}
			}
			InterlockedExchange(&m_pShared->peerCount, nCount);
			return nCount;
		}

		inline int32_t GetPeerCount()
		{
			if (!m_pShared)
			{
				return 0;
			}
			return m_pShared->peerCount;
		}

		// Iterates all connected peers (including self). fn(nSlotIndex, PeerSlot&) -> true to continue.
		template <typename Fn>
		inline void ForEachPeer(Fn&& fn)
		{
			if (!m_pShared)
			{
				return;
			}

			for (int i = 0; i < (int)MAX_PEERS; i++)
			{
				PeerSlot& slot = m_pShared->peerSlots[i];
				if (slot.connectState != 1)
				{
					continue;
				}
				if (!fn(i, slot))
				{
					break;
				}
			}
		}

		// Iterates all peers except the leader
		template <typename Fn>
		inline void ForEachFollower(Fn&& fn)
		{
			if (!m_pShared)
			{
				return;
			}

			int32_t nLeader = m_pShared->leaderSlot;
			for (int i = 0; i < (int)MAX_PEERS; i++)
			{
				if (i == nLeader)
				{
					continue;
				}
				PeerSlot& slot = m_pShared->peerSlots[i];
				if (slot.connectState != 1)
				{
					continue;
				}
				if (!fn(i, slot))
				{
					break;
				}
			}
		}

		inline int32_t GetMinPeerSpeed(uint32_t uWorldId)
		{
			int32_t nMinSpeed = 1000;
			ForEachPeer([&](int, PeerSlot& slot) -> bool
				{
					if (slot.worldId != uWorldId)
					{
						return true;
					}
					int32_t nSpeed = (int32_t)slot.speed;
					if (nSpeed == 0)
					{
						nSpeed = 10;
					}
					if (nSpeed < nMinSpeed)
					{
						nMinSpeed = nSpeed;
					}
					return true;
				});
			return nMinSpeed;
		}

		inline bool IsAnyPeerSlowed(uint32_t uWorldId)
		{
			bool bSlowed = false;
			ForEachPeer([&](int, PeerSlot& slot) -> bool
				{
					if (slot.worldId == uWorldId && slot.isSlowed)
					{
						bSlowed = true;
						return false;
					}
					return true;
				});
			return bSlowed;
		}

		inline void SnakeFollow(int32_t nServerObjectId)
		{
			int32_t nPrevObjectId = nServerObjectId;
			ForEachFollower([&](int nSlotIndex, PeerSlot& slot) -> bool
				{
					Cmd::FollowTarget cmdFollowTarget;
					cmdFollowTarget.nObjectId = nPrevObjectId;
					Send(CmdFollowTarget, cmdFollowTarget, TargetSlot(nSlotIndex));
					nPrevObjectId = slot.objectId;
					return true;
				});
		}

#pragma endregion

#pragma region Stale Peer Cleanup

		inline void PurgeStale()
		{
			if (!m_pShared)
			{
				return;
			}
			for (int i = 0; i < (int)MAX_PEERS; i++)
			{
				PeerSlot& slot = m_pShared->peerSlots[i];
				if (slot.connectState != 1)
				{
					continue;
				}

				HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, slot.processId);
				if (!hProc)
				{
					InterlockedExchange(&slot.connectState, 0);
					if (m_hPeerThreads[i])
					{
						CloseHandle(m_hPeerThreads[i]);
						m_hPeerThreads[i] = NULL;
					}

					InterlockedCompareExchange(&m_pShared->leaderSlot, -1, i);
					continue;
				}

				DWORD dwExitCode = STILL_ACTIVE;
				GetExitCodeProcess(hProc, &dwExitCode);
				CloseHandle(hProc);

				if (dwExitCode != STILL_ACTIVE)
				{
					InterlockedExchange(&slot.connectState, 0);
					if (m_hPeerThreads[i])
					{
						CloseHandle(m_hPeerThreads[i]);
						m_hPeerThreads[i] = NULL;
					}
					InterlockedCompareExchange(&m_pShared->leaderSlot, -1, i);
				}
			}
		}
#pragma endregion
	}
}
