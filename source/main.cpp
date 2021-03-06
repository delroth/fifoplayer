#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <map>
#include <vector>
#include <stdint.h>
#include <iostream>
#include <machine/endian.h>
#include <malloc.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <unistd.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;

#define DEF_ALIGN 32
class aligned_buf
{
public:
	aligned_buf() : buf(NULL), size(0), alignment(DEF_ALIGN) {}
	aligned_buf(int alignment) : buf(NULL), size(0), alignment(alignment) {}
	~aligned_buf()
	{
		free(buf);
	}

	aligned_buf(const aligned_buf& oth)
	{
		if (oth.buf)
		{
			buf = (u8*)memalign(oth.alignment, oth.size);
			printf("copied to %p (%x) \n", buf, MEM_VIRTUAL_TO_PHYSICAL(buf));
			memcpy(buf, oth.buf, oth.size);
		}
		else buf = NULL;
		size = oth.size;
		alignment = oth.alignment;
	}

	void resize(int new_size)
	{
		if (!buf)
		{
			buf = (u8*)memalign(alignment, new_size);
			printf("allocated to %p (%x) - size %x \n", buf, MEM_VIRTUAL_TO_PHYSICAL(buf), new_size);

		}
		else
		{
			u8* old_buf = buf;
			buf = (u8*)memalign(alignment, new_size);
			memcpy(buf, old_buf, std::min(new_size, size));
			printf("reallocated to %p (%x)\n", buf, MEM_VIRTUAL_TO_PHYSICAL(buf));
			free(old_buf);
		}
		size = new_size;
	}

	u8* buf;
	int size;

private:
	int alignment;
};

std::map<u32, aligned_buf > memory_map; // map of memory chunks (indexed by starting address)

bool IntersectsMemoryRange(u32 start1, u32 size1, u32 start2, u32 size2)
{
	return size1 && size2 && ((start1 >= start2 && start1 < start2 + size2) ||
			(start2 >= start1 && start2 < start1 + size1));
}

// TODO: Needs to take care of alignment, too!
// Returns true if memory layout changed
bool PrepareMemoryLoad(u32 start_addr, u32 size)
{
	bool ret = false;

	// Make sure alignment of data inside the memory block is preserved
	u32 off = start_addr % DEF_ALIGN;
	start_addr = start_addr - off;
	size += off;

	std::vector<u32> affected_elements;
	u32 new_start_addr = start_addr;
	u32 new_end_addr = start_addr + size - 1;

	// Find overlaps with existing memory chunks
	for (auto it = memory_map.begin(); it != memory_map.end(); ++it)
	{
		if (IntersectsMemoryRange(it->first, it->second.size, start_addr, size))
		{
			affected_elements.push_back(it->first);
			if (it->first < new_start_addr)
				new_start_addr = it->first;
			if (it->first + it->second.size > new_end_addr + 1)
				new_end_addr = it->first + it->second.size - 1;
		}
	}

	aligned_buf& new_memchunk(memory_map[new_start_addr]); // creates a new vector or uses the existing one
	u32 new_size = new_end_addr - new_start_addr + 1;

	// if the new memory range is inside an existing chunk, there's nothing to do
	if (new_memchunk.size == new_size)
		return false;

	// resize chunk to required size, move old content to it, replace old arrays with new one
	// NOTE: can't do reserve here because not the whole memory might be covered by existing memory chunks
	new_memchunk.resize(new_size);
	while (!affected_elements.empty())
	{
		u32 addr = affected_elements.back();

		// first chunk is already in new_memchunk
		if (addr != new_start_addr)
		{
			aligned_buf& src = memory_map[addr];
			memcpy(&new_memchunk.buf[addr - new_start_addr], &src.buf[0], src.size);
			memory_map.erase(addr);

			ret = true;
		}
		affected_elements.pop_back();
	}

	// TODO: Handle critical case where memory allocation fails!

	return ret;
}

// Must have been reserved via PrepareMemoryLoad first
u8* GetPointer(u32 addr)
{
	for (auto it = memory_map.begin(); it != memory_map.end(); ++it)
		if (addr >= it->first && addr < it->first + it->second.size)
			return &it->second.buf[addr - it->first];

	return NULL;
}

#if BYTE_ORDER==BIG_ENDIAN
uint64_t le64toh(uint64_t val)
{
	return ((val&0xff)<<56)|((val&0xff00)<<40)|((val&0xff0000)<<24)|((val&0xff000000)<<8) |
		((val&0xff00000000)>>8)|((val&0xff0000000000)>>24)|((val&0xff000000000000)>>40)|((val&0xff00000000000000)>>56);
}

uint32_t le32toh(uint32_t val)
{
	return ((val&0xff)<<24)|((val&0xff00)<<8)|((val&0xff0000)>>8)|((val&0xff000000)>>24);
}

uint32_t h32tole(uint32_t val)
{
	return le32toh(val);
}

uint16_t le16toh(uint16_t val)
{
	return ((val&0xff)<<8)|((val&0xff00)>>8);
}

uint64_t be64toh(uint64_t val)
{
	return val;
}

uint32_t be32toh(uint32_t val)
{
	return val;
}

uint16_t be16toh(uint16_t val)
{
	return val;
}

#elif BYTE_ORDER==LITTLE_ENDIAN
#error other stuff
#endif

#pragma pack(push, 4)

union DffFileHeader {
	struct {
		u32 fileId;
		u32 file_version;
		u32 min_loader_version;
		u64 bpMemOffset;
		u32 bpMemSize;
		u64 cpMemOffset;
		u32 cpMemSize;
		u64 xfMemOffset;
		u32 xfMemSize;
		u64 xfRegsOffset;
		u32 xfRegsSize;
		u64 frameListOffset;
		u32 frameCount;
		u32 flags;
	};
	u32 rawData[32];

	void FixEndianness()
	{
		fileId = le32toh(fileId);
		file_version = le32toh(file_version);
		min_loader_version = le32toh(min_loader_version);
		bpMemOffset = le64toh(bpMemOffset);
		bpMemSize = le32toh(bpMemSize);
		cpMemOffset = le64toh(cpMemOffset);
		cpMemSize = le32toh(cpMemSize);
		xfMemOffset = le64toh(xfMemOffset);
		xfMemSize = le32toh(xfMemSize);
		xfRegsOffset = le64toh(xfRegsOffset);
		xfRegsSize = le32toh(xfRegsSize);
		frameListOffset = le64toh(frameListOffset);
		frameCount = le32toh(frameCount);
		flags = le32toh(flags);
	}
};

union DffFrameInfo
{
	struct
	{
		u64 fifoDataOffset;
		u32 fifoDataSize;
		u32 fifoStart;
		u32 fifoEnd;
		u64 memoryUpdatesOffset;
		u32 numMemoryUpdates;
	};
	u32 rawData[16];

	void FixEndianness()
	{
		fifoDataOffset = le64toh(fifoDataOffset);
		fifoDataSize = le32toh(fifoDataSize);
		fifoStart = le32toh(fifoStart);
		fifoEnd = le32toh(fifoEnd);
		memoryUpdatesOffset = le64toh(memoryUpdatesOffset);
		numMemoryUpdates = le32toh(numMemoryUpdates);
	}
};

struct DffMemoryUpdate
{
	u32 fifoPosition;
	u32 address;
	u64 dataOffset;
	u32 dataSize;
	u8 type;

	void FixEndianness()
	{
		fifoPosition = le32toh(fifoPosition);
		address = le32toh(address);
		dataOffset = le64toh(dataOffset);
		dataSize = le32toh(dataSize);
	}
};

#define BPMEM_TRIGGER_EFB_COPY 0x52
#define BPMEM_CLEARBBOX1       0x55
#define BPMEM_CLEARBBOX2       0x56
#define BPMEM_CLEAR_PIXEL_PERF 0x57
#define BPMEM_SETDRAWDONE      0x45
#define BPMEM_PE_TOKEN_ID      0x47
#define BPMEM_PE_TOKEN_INT_ID  0x48
#define BPMEM_PRELOAD_MODE     0x63
#define BPMEM_LOADTLUT0        0x64
#define BPMEM_LOADTLUT1        0x65
#define BPMEM_TEXINVALIDATE    0x66



struct MemoryUpdate
{
	enum Type {
		TEXTURE_MAP = 0x01,
		XF_DATA = 0x02,
		VERTEX_STREAM = 0x04,
		TMEM = 0x08,
	};

	u32 fifoPosition;
	u32 address;
	std::vector<u8> data;
	Type type;
};

struct FifoFrameData
{
	std::vector<u8> fifoData;
	u32 fifoStart;
	u32 fifoEnd;

	// Sorted by position - TODO: Make this a map instead?
	std::vector<MemoryUpdate> memoryUpdates;
};

#include "BPMemory.h"
#define ENABLE_CONSOLE 0

static u32 tex_addr[8] = {0};

struct FifoData
{
	std::vector<FifoFrameData> frames;

	std::vector<u32> bpmem;
	std::vector<u32> cpmem;
	std::vector<u32> xfmem;
	std::vector<u32> xfregs;

	void ApplyInitialState()
	{
		for (unsigned int i = 0; i < bpmem.size(); ++i)
		{
			if ((i == BPMEM_TRIGGER_EFB_COPY
				|| i == BPMEM_CLEARBBOX1
				|| i == BPMEM_CLEARBBOX2
				|| i == BPMEM_SETDRAWDONE
				|| i == BPMEM_PE_TOKEN_ID // TODO: Sure that we want to skip this one?
				|| i == BPMEM_PE_TOKEN_INT_ID
				|| i == BPMEM_LOADTLUT0
				|| i == BPMEM_LOADTLUT1
				|| i == BPMEM_TEXINVALIDATE
				|| i == BPMEM_PRELOAD_MODE
				|| i == BPMEM_CLEAR_PIXEL_PERF))
				continue;

			u32 new_value = bpmem[i];

			// Patch texture addresses
			if ((i >= BPMEM_TX_SETIMAGE3 && i < BPMEM_TX_SETIMAGE3+4) ||
				(i >= BPMEM_TX_SETIMAGE3_4 && i < BPMEM_TX_SETIMAGE3_4+4))
			{
				u32 tempval = le32toh(new_value);
				TexImage3* img = (TexImage3*)&tempval;
				u32 addr = img->image_base << 5;
				u32 new_addr = MEM_VIRTUAL_TO_PHYSICAL(GetPointer(addr));
				img->image_base = new_addr >> 5;
				new_value = h32tole(tempval);

				if (i >= BPMEM_TX_SETIMAGE3 && i < BPMEM_TX_SETIMAGE3+4)
					tex_addr[i - BPMEM_TX_SETIMAGE3] = new_addr;
				else
					tex_addr[4 + i - BPMEM_TX_SETIMAGE3_4] = new_addr;
			}

#if ENABLE_CONSOLE!=1
			wgPipe->U8 = 0x61;
			wgPipe->U32 = (i<<24)|(le32toh(new_value)&0xffffff);
#endif
		}

#if ENABLE_CONSOLE!=1
		#define MLoadCPReg(addr, val) { wgPipe->U8 = 0x08; wgPipe->U8 = addr; wgPipe->U32 = val; }

		MLoadCPReg(0x30, le32toh(cpmem[0x30]));
		MLoadCPReg(0x40, le32toh(cpmem[0x40]));
		MLoadCPReg(0x50, le32toh(cpmem[0x50]));
		MLoadCPReg(0x60, le32toh(cpmem[0x60]));

		for (int i = 0; i < 8; ++i)
		{
			MLoadCPReg(0x70 + i, le32toh(cpmem[0x70 + i]));
			MLoadCPReg(0x80 + i, le32toh(cpmem[0x80 + i]));
			MLoadCPReg(0x90 + i, le32toh(cpmem[0x90 + i]));
		}

		for (int i = 0; i < 16; ++i)
		{
			MLoadCPReg(0xa0 + i, le32toh(cpmem[0xa0 + i]));
			MLoadCPReg(0xb0 + i, le32toh(cpmem[0xb0 + i]));
		}
		#undef MLoadCPReg

		for (unsigned int i = 0; i < xfmem.size(); i += 16)
		{
			wgPipe->U8 = 0x10;
			wgPipe->U32 = 0xf0000 | (i&0xffff); // load 16*4 bytes
			for (int k = 0; k < 16; ++k)
				wgPipe->U32 = le32toh(xfmem[i + k]);
		}

		for (unsigned int i = 0; i < xfregs.size(); ++i)
		{
			wgPipe->U8 = 0x10;
			wgPipe->U32 = 0x1000 | (i&0x0fff);
			wgPipe->U32 = le32toh(xfregs[i]);
		}

		// Flush WGP
		for (int i = 0; i < 7; ++i)
			wgPipe->U32 = 0;
		wgPipe->U16 = 0;
		wgPipe->U8 = 0;
#endif
	}
};

#include "fat.h"
#include <dirent.h>

//#define DFF_FILENAME "sd:/dff/4_efbcopies_new.dff"
//#define DFF_FILENAME "sd:/dff/3_textures_new.dff"
//#define DFF_FILENAME "sd:/dff/5_mkdd.dff"
//#define DFF_FILENAME "sd:/dff/smg_marioeyes.dff"
//#define DFF_FILENAME "sd:/dff/tmnt_fog.dff"
#define DFF_FILENAME "sd:/dff/ZeldaWW.dff"
//#define DFF_FILENAME "sd:/dff/simpletexture.dff"
//#define DFF_FILENAME "sd:/dff/fog_adj.dff"

void LoadDffData(FifoData& out)
{
	if(!fatInitDefault())
	{
		printf("fatInitDefault failed!\n");
	}

	u8* dff_data = NULL;

	off_t fsize = 0;

	struct stat st;
	if (stat (DFF_FILENAME, &st) == 0)
		fsize = st.st_size;

	dff_data = new u8[st.st_size];

	FILE* file = fopen(DFF_FILENAME, "r");
	if (!file)
		printf("Failed to open file!\n");

	size_t numread = fread(dff_data, st.st_size, 1, file);
	printf("Read %llx bytes\n", numread * st.st_size);

	DffFileHeader header;
	memcpy(&header,  &dff_data[0], sizeof(DffFileHeader));
	header.FixEndianness();

	if (header.fileId != 0x0d01f1f0 || header.min_loader_version > 1)
	{
		printf ("file ID or version don't match!\n");
	}
	printf ("Got %d frame%s\n", header.frameCount, (header.frameCount == 1) ? "" : "s");

	for (unsigned int i = 0;i < header.frameCount; ++i)
	{
		u64 frameOffset = header.frameListOffset + (i * sizeof(DffFrameInfo));
		DffFrameInfo srcFrame;
		memcpy(&srcFrame, &dff_data[frameOffset], sizeof(DffFrameInfo));
		srcFrame.FixEndianness();

//		printf("Frame %d got %d bytes of data (Start: 0x%#x, End: 0x%#x)\n", i, srcFrame.fifoDataSize, srcFrame.fifoStart, srcFrame.fifoEnd);

		out.frames.push_back(FifoFrameData());
		FifoFrameData& dstFrame = out.frames[i];
		// Skipping last 5 bytes, which are assumed to be a CopyDisp call for the XFB copy
		dstFrame.fifoData.reserve(srcFrame.fifoDataSize-5);
		dstFrame.fifoData.insert(dstFrame.fifoData.begin(), &dff_data[srcFrame.fifoDataOffset], &dff_data[srcFrame.fifoDataOffset]+srcFrame.fifoDataSize-5);

		u64 memoryUpdatesOffset;
		dstFrame.memoryUpdates.resize(srcFrame.numMemoryUpdates);
		for (unsigned int i = 0;i < srcFrame.numMemoryUpdates; ++i)
		{
			u64 updateOffset = srcFrame.memoryUpdatesOffset + (i * sizeof(DffMemoryUpdate));
			DffMemoryUpdate srcUpdate;
			memcpy(&srcUpdate, &dff_data[updateOffset], sizeof(DffMemoryUpdate));
			srcUpdate.FixEndianness();

			MemoryUpdate& dstUpdate = dstFrame.memoryUpdates[i];
			dstUpdate.address = srcUpdate.address;
			dstUpdate.fifoPosition = srcUpdate.fifoPosition;
			dstUpdate.data.resize(srcUpdate.dataSize);
			dstUpdate.type = (MemoryUpdate::Type) srcUpdate.type;

			memcpy(&dstUpdate.data[0], &dff_data[srcUpdate.dataOffset], srcUpdate.dataSize);
		}
	}

	// Save initial state
	u32 bp_size = header.bpMemSize;
	u32* bp_ptr = (u32*)&dff_data[header.bpMemOffset];
	out.bpmem.reserve(bp_size);
	out.bpmem.insert(out.bpmem.begin(), bp_ptr, bp_ptr + bp_size);

	u32 cp_size = header.cpMemSize;
	u32* cp_ptr = (u32*)&dff_data[header.cpMemOffset];
	out.cpmem.reserve(cp_size);
	out.cpmem.insert(out.cpmem.begin(), cp_ptr, cp_ptr + cp_size);

	u32 xf_size = header.xfMemSize;
	u32* xf_ptr = (u32*)&dff_data[header.xfMemOffset];
	out.xfmem.reserve(xf_size);
	out.xfmem.insert(out.xfmem.begin(), xf_ptr, xf_ptr + xf_size);

	u32 xf_regs_size = header.xfRegsSize;
	u32* xf_regs_ptr = (u32*)&dff_data[header.xfRegsOffset];
	out.xfregs.reserve(xf_regs_size);
	out.xfregs.insert(out.xfregs.begin(), xf_regs_ptr, xf_regs_ptr + xf_regs_size);

	delete[] dff_data;
}

struct AnalyzedFrameInfo
{
	std::vector<u32> object_starts;
	std::vector<u32> object_ends;
	std::vector<u32> cmd_starts;
//	std::vector<MemoryUpdate> memory_updates;
};

#include "OpcodeDecoding.h"
#include "FifoAnalyzer.h"


class FifoDataAnalyzer
{
public:
	void AnalyzeFrames(FifoData& data, std::vector<AnalyzedFrameInfo>& frame_info)
	{
		// TODO: Load BP mem

		u32 *cpMem = &data.cpmem[0];
		LoadCPReg(0x50, le32toh(cpMem[0x50]), m_cpmem);
		LoadCPReg(0x60, le32toh(cpMem[0x60]), m_cpmem);

		for (int i = 0; i < 8; ++i)
		{
			LoadCPReg(0x70 + i, le32toh(cpMem[0x70 + i]), m_cpmem);
			LoadCPReg(0x80 + i, le32toh(cpMem[0x80 + i]), m_cpmem);
			LoadCPReg(0x90 + i, le32toh(cpMem[0x90 + i]), m_cpmem);
		}

		frame_info.clear();
		frame_info.resize(data.frames.size());

		m_drawingObject = false;

		for (unsigned int frame_idx = 0; frame_idx < data.frames.size(); ++frame_idx)
		{
			FifoFrameData& src_frame = data.frames[frame_idx];
			AnalyzedFrameInfo& dst_frame = frame_info[frame_idx];

			u32 cmd_start = 0;

			while (cmd_start < src_frame.fifoData.size())
			{
				bool was_drawing = m_drawingObject;
				u32 cmd_size = DecodeCommand(&src_frame.fifoData[cmd_start]);

				// TODO: Check that cmd_size != 0

				if (was_drawing != m_drawingObject)
				{
					if (m_drawingObject)
						dst_frame.object_starts.push_back(cmd_start);
					else
						dst_frame.object_ends.push_back(cmd_start);
				}
				dst_frame.cmd_starts.push_back(cmd_start);
				cmd_start += cmd_size;
			}
			if (dst_frame.object_ends.size() < dst_frame.object_starts.size())
				dst_frame.object_ends.push_back(cmd_start);
		}
	}

	u32 DecodeCommand(u8* data)
	{
		u8* data_start = data;

		u8 cmd = ReadFifo8(data);

		static int stuff = 0;
//		printf("%02x ", cmd);
		++stuff;
//		if ((stuff % 16) == 15) printf("\n");
		switch (cmd)
		{
			case GX_NOP:
			case 0x44:
			case GX_CMD_INVL_VC:
				break;

			case GX_LOAD_CP_REG:
			{
				m_drawingObject = false;

				u32 cmd2 = ReadFifo8(data);
				u32 value = ReadFifo32(data);
				LoadCPReg(cmd2, value, m_cpmem);
				break;
			}

			case GX_LOAD_XF_REG:
			{
				m_drawingObject = false;

				u32 cmd2 = ReadFifo32(data);
				u8 stream_size = ((cmd2 >> 16) & 0xf) + 1; // TODO: Check if this works!

				data += stream_size * 4;
				break;
			}

			case GX_LOAD_INDX_A:
			case GX_LOAD_INDX_B:
			case GX_LOAD_INDX_C:
			case GX_LOAD_INDX_D:
				m_drawingObject = false;
				data += 4;
				break;

			case GX_CMD_CALL_DL:
				// The recorder should have expanded display lists into the fifo stream and skipped the call to start them
				// That is done to make it easier to track where memory is updated
				//_assert_(false);
				printf("Shouldn't have a DL here...\n");
				data += 8;
				break;

			case GX_LOAD_BP_REG:
			{
				m_drawingObject = false;

				u32 cmd2 = ReadFifo32(data);

//				printf("BP: %02x %08x\n", cmd, cmd2);
				//BPCmd bp = FifoAnalyzer::DecodeBPCmd(cmd2, m_BpMem); // TODO

				//FifoAnalyzer::LoadBPReg(bp, m_BpMem);
				// TODO: Load BP reg..

				// TODO
//				if (bp.address == BPMEM_TRIGGER_EFB_COPY)
//					StoreEfbCopyRegion();

				break;
			}

			default:
				if (cmd & 0x80)
				{
					m_drawingObject = true;
					u32 vtxAttrGroup = cmd & GX_VAT_MASK;
					int vertex_size = CalculateVertexSize(vtxAttrGroup, m_cpmem);

					u16 stream_size = ReadFifo16(data);
					data += stream_size * vertex_size;
				}
				else
				{
					printf("Invalid fifo command 0x%x\n", cmd);
					sleep(1);
				}
				break;
		}
		return data - data_start;
	}

private:
	bool m_drawingObject;

	CPMemory m_cpmem;
};

#pragma pack(pop)

#define DEFAULT_FIFO_SIZE   (256*1024)
static void *frameBuffer[2] = { NULL, NULL};
GXRModeObj *rmode;

u32 fb = 0;
u32 first_frame = 1;

void Init()
{
	VIDEO_Init();

	rmode = VIDEO_GetPreferredMode(NULL);
	first_frame = 1;
	fb = 0;
	frameBuffer[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode)); // TODO: Shouldn't require manual framebuffer management!
	frameBuffer[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(frameBuffer[fb]);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	fb ^= 1;

	void *gp_fifo = NULL;
	gp_fifo = memalign(32,DEFAULT_FIFO_SIZE);
	memset(gp_fifo,0,DEFAULT_FIFO_SIZE);

	GX_Init(gp_fifo,DEFAULT_FIFO_SIZE);

#if ENABLE_CONSOLE==1
    console_init(frameBuffer[0],20,20,rmode->fbWidth,rmode->xfbHeight,rmode->fbWidth*VI_DISPLAY_PIX_SZ);
#endif

	WPAD_Init();
}

#include "mygx.h"

int main()
{
	Init();

	FifoData fifo_data;
	LoadDffData(fifo_data);

	FifoDataAnalyzer analyzer;
	std::vector<AnalyzedFrameInfo> analyzed_frames;
	analyzer.AnalyzeFrames(fifo_data, analyzed_frames);

	bool processing = true;
	int first_frame = 0;
	int last_frame = first_frame + fifo_data.frames.size()-1;
	int cur_frame = first_frame;
	while (processing)
	{
		FifoFrameData& cur_frame_data = fifo_data.frames[cur_frame];
		AnalyzedFrameInfo& cur_analyzed_frame = analyzed_frames[cur_frame];
		if (cur_frame == 0) // TODO: Check for first_frame instead and apply previous state changes
		{
			for (unsigned int frameNum = 0; frameNum < fifo_data.frames.size(); ++frameNum)
			{
				const FifoFrameData &frame = fifo_data.frames[frameNum];
				for (unsigned int i = 0; i < frame.memoryUpdates.size(); ++i)
				{
					PrepareMemoryLoad(frame.memoryUpdates[i].address, frame.memoryUpdates[i].data.size());
					//if (early_mem_updates)
					//	memcpy(GetPointer(frame.memoryUpdates[i].address), &frame.memoryUpdates[i].data[0], frame.memoryUpdates[i].data.size());
					DCFlushRange(GetPointer(frame.memoryUpdates[i].address), frame.memoryUpdates[i].data.size());
				}
			}

			fifo_data.ApplyInitialState();
		}

		std::vector<u32>::iterator next_cmd_start = cur_analyzed_frame.cmd_starts.begin();

		u32 last_pos = 0;
		for (unsigned int i = 0; i < cur_frame_data.fifoData.size(); ++i)
		{
			const FifoFrameData &frame = fifo_data.frames[cur_frame];
			for (unsigned int update = 0; update < frame.memoryUpdates.size(); ++update)
			{
				if ((!last_pos || frame.memoryUpdates[update].fifoPosition > last_pos) && frame.memoryUpdates[update].fifoPosition <= i)
				{
					PrepareMemoryLoad(frame.memoryUpdates[update].address, frame.memoryUpdates[update].data.size());
					memcpy(GetPointer(frame.memoryUpdates[update].address), &frame.memoryUpdates[update].data[0], frame.memoryUpdates[update].data.size());

					// DCFlushRange expects aligned addresses
					u32 off = frame.memoryUpdates[update].address % DEF_ALIGN;
					DCFlushRange(GetPointer(frame.memoryUpdates[update].address - off), frame.memoryUpdates[update].data.size()+off);
				}
			}
			last_pos = i;

			bool skip_stuff = false;
			static u32 efbcopy_target = 0;
			if (next_cmd_start != cur_analyzed_frame.cmd_starts.end() && *next_cmd_start == i)
			{
				if (cur_frame_data.fifoData[i] == 0x61) // load BP reg
				{
					// Patch texture addresses
					if ((cur_frame_data.fifoData[i+1] >= BPMEM_TX_SETIMAGE3 && cur_frame_data.fifoData[i+1] < BPMEM_TX_SETIMAGE3+4) ||
						(cur_frame_data.fifoData[i+1] >= BPMEM_TX_SETIMAGE3_4 && cur_frame_data.fifoData[i+1] < BPMEM_TX_SETIMAGE3_4+4))
					{
						u32 tempval = /*be32toh*/(*(u32*)&cur_frame_data.fifoData[i+1]);
						TexImage3* img = (TexImage3*)&tempval;
						u32 addr = img->image_base << 5; // TODO: Proper mask?
						u32 new_addr = MEM_VIRTUAL_TO_PHYSICAL(GetPointer(addr));
						img->image_base = new_addr >> 5;
						u32 new_value = /*h32tobe*/(tempval);

						wgPipe->U8 = 0x61;
						wgPipe->U32 = ((u32)cur_frame_data.fifoData[i+1]<<24)|(/*be32toh*/(new_value)&0xffffff);

						i += 4;
						skip_stuff = true;

						if (cur_frame_data.fifoData[i+1] >= BPMEM_TX_SETIMAGE3 && cur_frame_data.fifoData[i+1] < BPMEM_TX_SETIMAGE3+4)
							tex_addr[cur_frame_data.fifoData[i+1] - BPMEM_TX_SETIMAGE3] = new_addr;
						else
							tex_addr[4 + cur_frame_data.fifoData[i+1] - BPMEM_TX_SETIMAGE3_4] = new_addr;
					}
					else if (cur_frame_data.fifoData[i+1] == BPMEM_PRELOAD_ADDR)
					{
						// TODO
					}
					else if (cur_frame_data.fifoData[i+1] == BPMEM_LOADTLUT0)
					{
#if 0
						u32 tempval = /*be32toh*/(*(u32*)&cur_frame_data.fifoData[i+1]);
						u32 addr = tempval << 5; // TODO: Proper mask?
						u32 new_addr = MEM_VIRTUAL_TO_PHYSICAL(GetPointer(addr));
						u32 new_value = new_addr >> 5;

						wgPipe->U8 = cur_frame_data.fifoData[i];
						wgPipe->U32 = (BPMEM_LOADTLUT0<<24)|(/*be32toh*/(new_value)&0xffffff);

						i += 4;
						skip_stuff = true;
#endif
					}
					else if (cur_frame_data.fifoData[i+1] == BPMEM_EFB_ADDR)
					{
						u32 tempval = /*be32toh*/(*(u32*)&cur_frame_data.fifoData[i+1]);
						u32 addr = (tempval & 0xFFFFFF) << 5; // TODO
						efbcopy_target = addr;
					}
					else if (cur_frame_data.fifoData[i+1] == BPMEM_TRIGGER_EFB_COPY)
					{
						u32 tempval = /*be32toh*/(*(u32*)&cur_frame_data.fifoData[i+1]);
						UPE_Copy* copy = (UPE_Copy*)&tempval;
						if (!copy->copy_to_xfb)
						{
							bool update_textures = PrepareMemoryLoad(efbcopy_target, 640*480*4); // TODO: Size!!
							u32 new_addr = MEM_VIRTUAL_TO_PHYSICAL(GetPointer(efbcopy_target));
							u32 new_value = /*h32tobe*/((BPMEM_EFB_ADDR<<24) | (new_addr >> 5));

							// Update target address
							wgPipe->U8 = 0x61;
							wgPipe->U32 = (BPMEM_EFB_ADDR<<24)|(/*be32toh*/(new_value)&0xffffff);

							// Gotta fix texture offsets if memory map layout changed
							if (update_textures)
							{
								for (int k = 0; k < 8; ++k)
								{
									u32 new_addr = MEM_VIRTUAL_TO_PHYSICAL(GetPointer(tex_addr[k]));
									u32 new_value = /*h32tobe*/(new_addr>>5);

									wgPipe->U8 = 0x61;
									u32 reg = (k < 4) ? (BPMEM_TX_SETIMAGE3+k) : (BPMEM_TX_SETIMAGE3_4+(k-4));
									wgPipe->U32 = (reg<<24)|(/*be32toh*/(new_value)&0xffffff);
								}
							}
						}
					}
				}
				else if (cur_frame_data.fifoData[i] == GX_LOAD_CP_REG)
				{
					if ((cur_frame_data.fifoData[i+1] & 0xF0) == 0xA0)
					{
						u32 old_addr = *(u32*)&cur_frame_data.fifoData[i+2];
						u32 new_addr = MEM_VIRTUAL_TO_PHYSICAL(GetPointer(old_addr));
						wgPipe->U8 = GX_LOAD_CP_REG;
						wgPipe->U8 = cur_frame_data.fifoData[i+1];
						wgPipe->U32 = new_addr;
						skip_stuff = true;
						i += 5;
					}
				}
				++next_cmd_start;
			}
#if ENABLE_CONSOLE!=1
			if (!skip_stuff)
				wgPipe->U8 = cur_frame_data.fifoData[i];
#endif
		}

#if ENABLE_CONSOLE!=1
		// finish frame
		// Note that GX_CopyDisp(frameBuffer[fb],GX_TRUE) uses an internal state
		// which is out of sync with the dff_data, so we're manually writing
		// to the EFB copy registers instead.
		wgPipe->U8 = GX_LOAD_BP_REG;
		wgPipe->U32 = (BPMEM_EFB_ADDR << 24) | ((MEM_VIRTUAL_TO_PHYSICAL(frameBuffer[fb]) >> 5) & 0xFFFFFF);

		UPE_Copy copy;
		copy.Hex = 0;
		copy.clear = 1;
		copy.copy_to_xfb = 1;
		wgPipe->U8 = GX_LOAD_BP_REG;
		wgPipe->U32 = (BPMEM_TRIGGER_EFB_COPY << 24) | copy.Hex;

		VIDEO_SetNextFramebuffer(frameBuffer[fb]);
		if (first_frame)
		{
			VIDEO_SetBlack(FALSE);
			first_frame = 0;
		}

		VIDEO_Flush();
#endif
		VIDEO_WaitVSync();
		fb ^= 1;

		// TODO: Menu stuff
		// reset GX state
		// draw menu
		// restore GX state

		// input checking
		// A = select menu point
		// B = menu back
		// plus = pause
		// minus = hide menu
		// home = stop
		WPAD_ScanPads();

//		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
//			processing = false;

		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
		{
			printf("\n");
			for (unsigned int i = 0; i < fifo_data.frames[0].fifoData.size(); ++i)
			{
				printf("%02x", fifo_data.frames[0].fifoData[i]);
				if (i == fifo_data.frames[0].fifoData.size()-5) printf("_");
//				if ((i % 4) == 3) printf(" ");
//				if ((i % 16) == 15) printf("\n");
				if ((i % 4) == 3) printf(" ");
				if ((i % 24) == 23) printf("\n");
			}
			exit(0);
		}

		++cur_frame;
		cur_frame = first_frame + ((cur_frame-first_frame) % (last_frame-first_frame+1));
	}

	return 0;
}
