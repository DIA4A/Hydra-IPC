/*
 * Hydra IPC Library
 * Copyright (c) 2026 DIA4A
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this project.
 */

#pragma once
#include <cstdint>
#include <cstring>
#include <Windows.h>

namespace HydraIPC
{
	using PeerMask = uint64_t;
	constexpr uint32_t MAX_PEERS            = sizeof(PeerMask) * 8;
	constexpr uint32_t COMMAND_RING_SIZE    = 256;
	constexpr uint32_t COMMAND_RING_MASK    = COMMAND_RING_SIZE - 1;
	constexpr uint32_t MAX_PAYLOAD_SIZE     = 512;
	constexpr uint32_t SHARED_MEMORY_MAGIC  = 'D4AM';
	constexpr uint32_t SHARED_MEMORY_VERSION = 3;

	inline const char* SharedMemoryName() { return "DIA4A_HydraIPC_SharedMem"; }

	constexpr PeerMask TargetAll = ~PeerMask(0);

	inline PeerMask TargetSlot(int32_t nSlot)
	{
		return (nSlot >= 0 && nSlot < (int32_t)MAX_PEERS) ? (PeerMask(1) << nSlot) : 0;
	}

	inline PeerMask TargetAllExcept(int32_t nSlot)
	{
		return ~TargetSlot(nSlot);
	}

	enum CommandType : uint32_t
	{
		CmdNone = 0,
		CmdNexus,
		CmdUsePortal,
		CmdCurrentWorld,
		CmdIngameDisconnect,
		CmdForceNexus,
		CmdChangeServer,
		CmdServerIPConnect,
		CmdUnplug,
		CmdLoadConfig,
		CmdRunCommand,
		CmdChatMessage,
		CmdFollowTarget,
		CmdRequestTome,
		CmdSwapout,
		CmdUseAbility,
		CmdInformObjectId,
		CmdBagDrop,

		CmdUserDefined = 0x1000,
	};

	namespace Cmd
	{
		struct UsePortal
		{
			int32_t nObjectId = 0;
		};

		struct CurrentWorld
		{
			uint32_t uWorldId = 0;
		};

		struct ChangeServer
		{
			int32_t nServerIndex = 0;
		};

		struct ServerIPConnect
		{
			char szServerIP[64] = {};
		};

		struct Unplug
		{
			bool bEnable = false;
		};

		struct LoadConfig
		{
			int32_t nConfigIndex = 0;
		};

		struct RunCommand
		{
			char szCommand[512] = {};
		};

		struct ChatMessage
		{
			char szMessage[256] = {};
		};

		struct FollowTarget
		{
			int32_t nObjectId = 0;
		};

		struct Swapout
		{
			int32_t nSwapToObjectType = -1;
		};

		struct InformObjectId
		{
			int32_t nObjectId = 0;
		};

		struct UseAbility
		{
			float flX = 0.f;
			float flY = 0.f;
		};

		struct BagDrop
		{
			int32_t nBagType;
			int32_t nBagObjectType;
			int32_t nItems[8];
			float flX, flY;
		};
	}

	struct CommandEntry
	{
		volatile uint32_t sequence; // nonzero when valid, matches (ringIndex + 1)
		uint32_t commandType;
		uint32_t senderSlot;
		PeerMask targetMask;
		uint32_t payloadSize;
		uint8_t  payload[MAX_PAYLOAD_SIZE];
	};

	struct alignas(64) LeaderBroadcastState
	{
		volatile long sequence; // seqlock: odd = writing, even = stable

		float posX, posY;

		bool shouldShoot;
		float shootX, shootY;

		uint32_t leaderWorldId;
		char leaderServerAddress[64];
		int32_t leaderObjectId;
		char leaderName[64];
		bool allowCrossWorldConnections;

		bool bShouldRequestTome = false;
		float flRequestTomeThreshold = 0.f;
		bool bNotifyBagTypes[6] = {};
		bool bShowBagContents = false;
		bool bInteract = false;
		int32_t nOptimizationMode = 0;
	};

	struct alignas(64) PeerSlot
	{
		// Registration (set on Join, cleared on Leave)
		volatile long connectState; // 0=empty, 1=connected
		uint32_t processId;
		uint32_t apcThreadId;
		uintptr_t apcCallbackAddr; // Address of APCCallback in this process

		// Peer state (updated each tick, read by leader/others)
		float speed;
		int32_t objectId;
		uint32_t worldId;
		volatile long isSlowed;
		volatile long isConnectedInGame;
		char name[20];
		int32_t inventory[28];
		int32_t backpackSlots;
	};

	struct CommandRingBuffer
	{
		alignas(64) volatile long writeIndex;
		CommandEntry entries[COMMAND_RING_SIZE];
	};

	struct SharedMemoryLayout
	{
		uint32_t magic;
		uint32_t version;
		volatile long isActive;
		volatile long leaderSlot;
		volatile long peerCount;

		alignas(64) LeaderBroadcastState leaderState;
		alignas(64) PeerSlot peerSlots[MAX_PEERS];
		alignas(64) CommandRingBuffer commandRing;
	};

	__declspec(noinline) inline void NTAPI APCCallback(ULONG_PTR) { }

	// Atomically test and clear a pending flag, returns true if the flag was set
	inline bool ConsumePending(volatile long& flag)
	{
		return InterlockedCompareExchange(&flag, 0, 1) == 1;
	}

	namespace Seqlock
	{
		inline void BeginWrite(volatile long& nSeq)
		{
			InterlockedIncrement(&nSeq);
			MemoryBarrier();
		}

		inline void EndWrite(volatile long& nSeq)
		{
			MemoryBarrier();
			InterlockedIncrement(&nSeq);
		}

		inline long BeginRead(const volatile long& nSeq)
		{
			long nVal;
			do
			{
				nVal = nSeq;
				MemoryBarrier();
			} while (nVal & 1);
			return nVal;
		}

		inline bool ValidateRead(const volatile long& nSeq, long nStartSeq)
		{
			MemoryBarrier();
			return nSeq == nStartSeq;
		}
	}
}
