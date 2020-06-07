#ifndef __TRACYD3D12_HPP__
#define __TRACYD3D12_HPP__

#ifndef TRACY_ENABLE

#define TracyD3D12Context(device, queue) nullptr
#define TracyD3D12Destroy(ctx)

#define TracyD3D12NamedZone(ctx, varname, cmdList, name, active)
#define TracyD3D12NamedZoneC(ctx, varname, cmdList, name, color, active)
#define TracyD3D12Zone(ctx, cmdList, name)
#define TracyD3D12ZoneC(ctx, cmdList, name, color)

#define TracyD3D12Collect(ctx)

namespace tracy
{
	class D3D12ZoneScope {};
}

using TracyD3D12Ctx = void*;

#else

#include "Tracy.hpp"
#include "client/TracyProfiler.hpp"

#include <cstdlib>
#include <cassert>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace tracy
{

	// Command queue context.
	class D3D12QueueCtx
	{
		friend class D3D12ZoneScope;

		static constexpr uint32_t MaxQueries = 64 * 1024;  // Queries are begin and end markers, so we can store half as many total time durations.

		bool m_initialized = false;

		ID3D12Device* m_device;
		uint8_t m_context;
		Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
		Microsoft::WRL::ComPtr<ID3D12Resource> m_readbackBuffer;
		
		uint32_t m_queryLimit = MaxQueries;
		uint32_t m_queryCounter = 0;
		uint32_t m_previousQueryCounter = 0;

	public:
		D3D12QueueCtx(ID3D12Device* device, ID3D12CommandQueue* queue)
			: m_device(device)
			, m_context(GetGpuCtxCounter().fetch_add(1, std::memory_order_relaxed))
		{
			// Verify we support timestamp queries on this queue.

			if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COPY)
			{
				D3D12_FEATURE_DATA_D3D12_OPTIONS3 featureData{};

				if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &featureData, sizeof(featureData))))
				{
					assert(false && "Platform does not support profiling of copy queues.");
				}
			}

			uint64_t timestampFrequency;

			if (FAILED(queue->GetTimestampFrequency(&timestampFrequency)))
			{
				assert(false && "Failed to get timestamp frequency.");
			}

			uint64_t cpuTimestamp;
			uint64_t gpuTimestamp;

			if (FAILED(queue->GetClockCalibration(&gpuTimestamp, &cpuTimestamp)))
			{
				assert(false && "Failed to get queue clock calibration.");
			}

			cpuTimestamp = Profiler::GetTime();

			D3D12_QUERY_HEAP_DESC heapDesc{};
			heapDesc.Type = queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			heapDesc.Count = m_queryLimit;
			heapDesc.NodeMask = 0;  // #TODO: Support multiple adapters.

			while (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_queryHeap))))
			{
				m_queryLimit /= 2;
				heapDesc.Count = m_queryLimit;
			}

			// Create a readback buffer, which will be used as a destination for the query data.

			D3D12_RESOURCE_DESC readbackBufferDesc{};
			readbackBufferDesc.Alignment = 0;
			readbackBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			readbackBufferDesc.Width = m_queryLimit * sizeof(uint64_t);
			readbackBufferDesc.Height = 1;
			readbackBufferDesc.DepthOrArraySize = 1;
			readbackBufferDesc.Format = DXGI_FORMAT_UNKNOWN;
			readbackBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;  // Buffers are always row major.
			readbackBufferDesc.MipLevels = 1;
			readbackBufferDesc.SampleDesc.Count = 1;
			readbackBufferDesc.SampleDesc.Quality = 0;
			readbackBufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			D3D12_HEAP_PROPERTIES readbackHeapProps{};
			readbackHeapProps.Type = D3D12_HEAP_TYPE_READBACK;
			readbackHeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			readbackHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
			readbackHeapProps.CreationNodeMask = 0;
			readbackHeapProps.VisibleNodeMask = 0;  // #TODO: Support multiple adapters.

			if (FAILED(device->CreateCommittedResource(&readbackHeapProps, D3D12_HEAP_FLAG_NONE, &readbackBufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readbackBuffer))))
			{
				assert(false && "Failed to create query readback buffer.");
			}

			auto* item = Profiler::QueueSerial();
			MemWrite(&item->hdr.type, QueueType::GpuNewContext);
			MemWrite(&item->gpuNewContext.cpuTime, cpuTimestamp);
			MemWrite(&item->gpuNewContext.gpuTime, gpuTimestamp);
			memset(&item->gpuNewContext.thread, 0, sizeof(item->gpuNewContext.thread));
			MemWrite(&item->gpuNewContext.period, 1E+09f / static_cast<float>(timestampFrequency));
			MemWrite(&item->gpuNewContext.context, m_context);
			MemWrite(&item->gpuNewContext.accuracyBits, uint8_t{ 0 });
			MemWrite(&item->gpuNewContext.type, GpuContextType::Vulkan);  // #TEMP: Add a Direct3D12 context type in the server.

#ifdef TRACY_ON_DEMAND
			GetProfiler().DeferItem(*item);
#endif

			Profiler::QueueSerialFinish();

			m_initialized = true;
		}

		~D3D12QueueCtx() {}

		void Collect()
		{
			ZoneScopedC(Color::Red4);

			// Check to see if we have any new queries.
			if (m_queryCounter == m_previousQueryCounter) return;

#ifdef TRACY_ON_DEMAND
			if (!GetProfiler().IsConnected())
			{
				m_queryCounter = 0;

				return;
			}
#endif

			// Batch submit all of our query data to the profiler.

			// Map the readback buffer so we can fetch the query data from the GPU.
			void* readbackBufferMapping = nullptr;

			if (FAILED(m_readbackBuffer->Map(0, nullptr, &readbackBufferMapping)))
			{
				assert(false && "Failed to map readback buffer.");
			}

			auto* timestampData = static_cast<uint64_t*>(readbackBufferMapping);

			for (uint32_t index = 0; index < m_queryCounter; ++index)
			{
				const auto timestamp = timestampData[(m_previousQueryCounter + index) % m_queryLimit];
				const auto queryId = m_previousQueryCounter + index;

				auto* item = Profiler::QueueSerial();
				MemWrite(&item->hdr.type, QueueType::GpuTime);
				MemWrite(&item->gpuTime.gpuTime, timestamp);
				MemWrite(&item->gpuTime.queryId, static_cast<uint16_t>(queryId));
				MemWrite(&item->gpuTime.context, m_context);

				Profiler::QueueSerialFinish();
			}

			m_readbackBuffer->Unmap(0, nullptr);

			m_previousQueryCounter += m_queryCounter;
			m_queryCounter = 0;

			if (m_previousQueryCounter >= m_queryLimit)
			{
				m_previousQueryCounter -= m_queryLimit;
			}
		}

	private:
		tracy_force_inline uint32_t NextQueryId()
		{
			assert(m_queryCounter < m_queryLimit && "Submitted too many GPU queries! Consider increasing MaxQueries.");

			const uint32_t id = (m_previousQueryCounter + m_queryCounter) % m_queryLimit;
			++m_queryCounter;

			return id;
		}

		tracy_force_inline uint8_t GetId() const
		{
			return m_context;
		}
	};

	class D3D12ZoneScope
	{
		const bool m_active;
		D3D12QueueCtx* m_ctx = nullptr;
		ID3D12GraphicsCommandList* m_cmdList = nullptr;

	public:
		tracy_force_inline D3D12ZoneScope(D3D12QueueCtx* ctx, ID3D12GraphicsCommandList* cmdList, const SourceLocationData* srcLocation, bool active)
#ifdef TRACY_ON_DEMAND
			: m_active(active && GetProfiler().IsConnected())
#else
			: m_active(active)
#endif
		{
			if (!m_active) return;

			m_ctx = ctx;
			m_cmdList = cmdList;

			const auto queryId = ctx->NextQueryId();
			cmdList->EndQuery(ctx->m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryId);

			auto* item = Profiler::QueueSerial();
#if defined(TRACY_HAS_CALLSTACK) && defined(TRACY_CALLSTACK)
			MemWrite(&item->hdr.type, QueueType::GpuZoneBeginCallstackSerial);
#else
			MemWrite(&item->hdr.type, QueueType::GpuZoneBeginSerial);
#endif
			MemWrite(&item->gpuZoneBegin.cpuTime, Profiler::GetTime());
			MemWrite(&item->gpuZoneBegin.srcloc, reinterpret_cast<uint64_t>(srcLocation));
			MemWrite(&item->gpuZoneBegin.thread, GetThreadHandle());
			MemWrite(&item->gpuZoneBegin.queryId, static_cast<uint16_t>(queryId));
			MemWrite(&item->gpuZoneBegin.context, ctx->GetId());

			Profiler::QueueSerialFinish();

#if defined(TRACY_HAS_CALLSTACK) && defined(TRACY_CALLSTACK)
			GetProfiler().SendCallstack(TRACY_CALLSTACK);
#endif
		}

		tracy_force_inline ~D3D12ZoneScope()
		{
			if (!m_active) return;

			const auto queryId = m_ctx->NextQueryId();
			m_cmdList->EndQuery(m_ctx->m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryId);

			auto* item = Profiler::QueueSerial();
			MemWrite(&item->hdr.type, QueueType::GpuZoneEndSerial);
			MemWrite(&item->gpuZoneEnd.cpuTime, Profiler::GetTime());
			MemWrite(&item->gpuZoneEnd.thread, GetThreadHandle());
			MemWrite(&item->gpuZoneEnd.queryId, static_cast<uint16_t>(queryId));
			MemWrite(&item->gpuZoneEnd.context, m_ctx->GetId());

			Profiler::QueueSerialFinish();

			m_cmdList->ResolveQueryData(m_ctx->m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryId - 1, 2, m_ctx->m_readbackBuffer.Get(), (queryId - 1) * sizeof(uint64_t));
		}
	};

	static inline D3D12QueueCtx* CreateD3D12Context(ID3D12Device* device, ID3D12CommandQueue* queue)
	{
		InitRPMallocThread();

		auto* ctx = static_cast<D3D12QueueCtx*>(tracy_malloc(sizeof(D3D12QueueCtx)));
		new (ctx) D3D12QueueCtx{ device, queue };

		return ctx;
	}

	static inline void DestroyD3D12Context(D3D12QueueCtx* ctx)
	{
		ctx->~D3D12QueueCtx();
		tracy_free(ctx);
	}

}

using TracyD3D12Ctx = tracy::D3D12QueueCtx*;

#define TracyD3D12Context(device, queue) tracy::CreateD3D12Context(device, queue);
#define TracyD3D12Destroy(ctx) tracy::DestroyD3D12Context(ctx);

#define TracyD3D12NamedZone(ctx, varname, cmdList, name, active) static const tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location, __LINE__) { name, __FUNCTION__, __FILE__, (uint32_t)__LINE__, 0 }; tracy::D3D12ZoneScope varname{ ctx, cmdList, &TracyConcat(__tracy_gpu_source_location, __LINE__), active };
#define TracyD3D12NamedZoneC(ctx, varname, cmdList, name, color, active) static const tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location, __LINE__) { name, __FUNCTION__, __FILE__, (uint32_t)__LINE__, color }; tracy::D3D12ZoneScope varname{ ctx, cmdList, &TracyConcat(__tracy_gpu_source_location, __LINE__), active };
#define TracyD3D12Zone(ctx, cmdList, name) TracyD3D12NamedZone(ctx, ___tracy_gpu_zone, cmdList, name, true)
#define TracyD3D12ZoneC(ctx, cmdList, name, color) TracyD3D12NamedZoneC(ctx, ___tracy_gpu_zone, cmdList, name, color, true)

#define TracyD3D12Collect(ctx) ctx->Collect();

#endif

#endif
