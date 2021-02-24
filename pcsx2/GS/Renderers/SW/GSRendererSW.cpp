/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"
#include "GSRendererSW.h"

#define LOG 0

static FILE* s_fp = LOG ? fopen("c:\\temp1\\_.txt", "w") : NULL;

CONSTINIT const GSVector4 GSRendererSW::m_pos_scale = GSVector4::cxpr(1.0f / 16, 1.0f / 16, 1.0f, 128.0f);
#if _M_SSE >= 0x501
CONSTINIT const GSVector8 GSRendererSW::m_pos_scale2 = GSVector8::cxpr(1.0f / 16, 1.0f / 16, 1.0f, 128.0f, 1.0f / 16, 1.0f / 16, 1.0f, 128.0f);
#endif

GSRendererSW::GSRendererSW(int threads)
	: m_fzb(NULL)
{
	m_nativeres = true; // ignore ini, sw is always native

	m_tc = new GSTextureCacheSW(this);

	memset(m_texture, 0, sizeof(m_texture));

	m_rl = GSRasterizerList::Create<GSDrawScanline>(threads, &m_perfmon);

	m_output = (uint8*)_aligned_malloc(1024 * 1024 * sizeof(uint32), 32);

	for (uint32 i = 0; i < countof(m_fzb_pages); i++)
	{
		m_fzb_pages[i] = 0;
	}
	for (uint32 i = 0; i < countof(m_tex_pages); i++)
	{
		m_tex_pages[i] = 0;
	}

	#define InitCVB2(P, Q) \
		m_cvb[P][0][0][Q] = &GSRendererSW::ConvertVertexBuffer<P, 0, 0, Q>; \
		m_cvb[P][0][1][Q] = &GSRendererSW::ConvertVertexBuffer<P, 0, 1, Q>; \
		m_cvb[P][1][0][Q] = &GSRendererSW::ConvertVertexBuffer<P, 1, 0, Q>; \
		m_cvb[P][1][1][Q] = &GSRendererSW::ConvertVertexBuffer<P, 1, 1, Q>;

	#define InitCVB(P) \
		InitCVB2(P, 0) \
		InitCVB2(P, 1)

	InitCVB(GS_POINT_CLASS);
	InitCVB(GS_LINE_CLASS);
	InitCVB(GS_TRIANGLE_CLASS);
	InitCVB(GS_SPRITE_CLASS);

	m_dump_root = root_sw;

	// Reset handler with the auto flush hack enabled on the SW renderer.
	// Some games run better without the hack so rely on ini/gui option.
	if (!GLLoader::in_replayer && theApp.GetConfigB("autoflush_sw"))
	{
		m_userhacks_auto_flush = true;
		ResetHandlers();
	}
}

GSRendererSW::~GSRendererSW()
{
	delete m_tc;

	for (size_t i = 0; i < countof(m_texture); i++)
	{
		delete m_texture[i];
	}

	delete m_rl;

	_aligned_free(m_output);
}

void GSRendererSW::Reset()
{
	Sync(-1);

	m_tc->RemoveAll();

	GSRenderer::Reset();
}

void GSRendererSW::VSync(int field)
{
	Sync(0); // IncAge might delete a cached texture in use

	if (0) if (LOG)
	{
		fprintf(s_fp, "%llu\n", m_perfmon.GetFrame());

		GSVector4i dr = GetDisplayRect();
		GSVector4i fr = GetFrameRect();

		fprintf(s_fp, "dr %d %d %d %d, fr %d %d %d %d\n",
			dr.x, dr.y, dr.z, dr.w,
			fr.x, fr.y, fr.z, fr.w);

		m_regs->Dump(s_fp);

		fflush(s_fp);
	}

	/*
	int draw[8], sum = 0;

	for(size_t i = 0; i < countof(draw); i++)
	{
		draw[i] = m_perfmon.CPU(GSPerfMon::WorkerDraw0 + i);
		sum += draw[i];
	}

	printf("CPU %d Sync %d W %d %d %d %d %d %d %d %d (%d)\n",
		m_perfmon.CPU(GSPerfMon::Main),
		m_perfmon.CPU(GSPerfMon::Sync),
		draw[0], draw[1], draw[2], draw[3], draw[4], draw[5], draw[6], draw[7], sum);

	//
	*/

	GSRenderer::VSync(field);

	m_tc->IncAge();

	// if((m_perfmon.GetFrame() & 255) == 0) m_rl->PrintStats();
}

void GSRendererSW::ResetDevice()
{
	for (size_t i = 0; i < countof(m_texture); i++)
	{
		delete m_texture[i];

		m_texture[i] = NULL;
	}
}

GSTexture* GSRendererSW::GetOutput(int i, int& y_offset)
{
	Sync(1);

	const GSRegDISPFB& DISPFB = m_regs->DISP[i].DISPFB;

	int w = DISPFB.FBW * 64;
	int h = GetFramebufferHeight();

	// TODO: round up bottom

	if (m_dev->ResizeTexture(&m_texture[i], w, h))
	{
		static int pitch = 1024 * 4;

		GSVector4i r(0, 0, w, h);

		const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[DISPFB.PSM];

		(m_mem.*psm.rtx)(m_mem.GetOffset(DISPFB.Block(), DISPFB.FBW, DISPFB.PSM), r.ralign<Align_Outside>(psm.bs), m_output, pitch, m_env.TEXA);

		m_texture[i]->Update(r, m_output, pitch);

		if (s_dump)
		{
			if (s_savef && s_n >= s_saven)
			{
				m_texture[i]->Save(m_dump_root + format("%05d_f%lld_fr%d_%05x_%s.bmp", s_n, m_perfmon.GetFrame(), i, (int)DISPFB.Block(), psm_str(DISPFB.PSM)));
			}
		}
	}

	return m_texture[i];
}

GSTexture* GSRendererSW::GetFeedbackOutput()
{
	int dummy;

	// It is enough to emulate Xenosaga cutscene. (or any game that will do a basic loopback)
	for (int i = 0; i < 2; i++)
	{
		if (m_regs->EXTBUF.EXBP == m_regs->DISP[i].DISPFB.Block())
			return GetOutput(i, dummy);
	}

	return nullptr;
}


template <uint32 primclass, uint32 tme, uint32 fst, uint32 q_div>
void GSRendererSW::ConvertVertexBuffer(GSVertexSW* RESTRICT dst, const GSVertex* RESTRICT src, size_t count)
{
	// FIXME q_div wasn't added to AVX2 code path.

#if 0 //_M_SSE >= 0x501

	// TODO: something isn't right here, this makes other functions slower (split load/store? old sse code in 3rd party lib?)

	GSVector8i o2((GSVector4i)m_context->XYOFFSET);
	GSVector8 tsize2(GSVector4(0x10000 << m_context->TEX0.TW, 0x10000 << m_context->TEX0.TH, 1, 0));

	for(int i = (int)m_vertex.next; i > 0; i -= 2, src += 2, dst += 2) // ok to overflow, allocator makes sure there is one more dummy vertex
	{
		GSVector8i v0 = GSVector8i::load<true>(src[0].m);
		GSVector8i v1 = GSVector8i::load<true>(src[1].m);

		GSVector8 stcq = GSVector8::cast(v0.ac(v1));
		GSVector8i xyzuvf = v0.bd(v1);

		//GSVector8 stcq = GSVector8::load(&src[0].m[0], &src[1].m[0]);
		//GSVector8i xyzuvf = GSVector8i::load(&src[0].m[1], &src[1].m[1]);

		GSVector8i xy = xyzuvf.upl16() - o2;
		GSVector8i zf = xyzuvf.ywww().min_u32(GSVector8i::xffffff00());

		GSVector8 p = GSVector8(xy).xyxy(GSVector8(zf) + (GSVector8::m_x4f800000 & GSVector8::cast(zf.sra32(31)))) * m_pos_scale2;
		GSVector8 c = GSVector8(GSVector8i::cast(stcq).uph8().upl16() << 7);

		GSVector8 t = GSVector8::zero();

		if(tme)
		{
			if(fst)
			{
				t = GSVector8(xyzuvf.uph16() << (16 - 4));
			}
			else
			{
				t = stcq.xyww() * tsize2;
			}
		}

		if(primclass == GS_SPRITE_CLASS)
		{
			t = t.insert32<1, 3>(GSVector8::cast(xyzuvf));
		}

		GSVector8::storel(&dst[0].p, p);

		if(tme || primclass == GS_SPRITE_CLASS)
		{
			GSVector8::store<true>(&dst[0].t, t.ac(c));
		}
		else
		{
			GSVector8::storel(&dst[0].c, c);
		}

		GSVector8::storeh(&dst[1].p, p);

		if(tme || primclass == GS_SPRITE_CLASS)
		{
			GSVector8::store<true>(&dst[1].t, t.bd(c));
		}
		else
		{
			GSVector8::storeh(&dst[1].c, c);
		}
	}

#else

	GSVector4i off = (GSVector4i)m_context->XYOFFSET;
	GSVector4 tsize = GSVector4(0x10000 << m_context->TEX0.TW, 0x10000 << m_context->TEX0.TH, 1, 0);
	GSVector4i z_max = GSVector4i::xffffffff().srl32(GSLocalMemory::m_psm[m_context->ZBUF.PSM].fmt * 8);

	for (int i = (int)m_vertex.next; i > 0; i--, src++, dst++)
	{
		GSVector4 stcq = GSVector4::load<true>(&src->m[0]); // s t rgba q

		GSVector4i xyzuvf(src->m[1]);

		GSVector4i xy = xyzuvf.upl16() - off;
		GSVector4i zf = xyzuvf.ywww().min_u32(GSVector4i::xffffff00());

		dst->p = GSVector4(xy).xyxy(GSVector4(zf) + (GSVector4::m_x4f800000 & GSVector4::cast(zf.sra32(31)))) * m_pos_scale;
		dst->c = GSVector4(GSVector4i::cast(stcq).zzzz().u8to32() << 7);

		GSVector4 t = GSVector4::zero();

		if (tme)
		{
			if (fst)
			{
				t = GSVector4(xyzuvf.uph16() << (16 - 4));
			}
			else if (q_div)
			{
				// Division is required if number are huge (Pro Soccer Club)
				if (primclass == GS_SPRITE_CLASS && (i & 1) == 0)
				{
					// q(n) isn't valid, you need to take q(n+1)
					const GSVertex* next = src + 1;
					GSVector4 stcq1 = GSVector4::load<true>(&next->m[0]); // s t rgba q
					t = (stcq / stcq1.wwww()) * tsize;
				}
				else
				{
					t = (stcq / stcq.wwww()) * tsize;
				}
			}
			else
			{
				t = stcq.xyww() * tsize;
			}
		}

		if (primclass == GS_SPRITE_CLASS)
		{
			xyzuvf = xyzuvf.min_u32(z_max);
			t = t.insert32<1, 3>(GSVector4::cast(xyzuvf));
		}

		dst->t = t;

#if 0 //_M_SSE >= 0x501

		dst->_pad = GSVector4::zero();

#endif
	}

#endif
}

void GSRendererSW::Draw()
{
	const GSDrawingContext* context = m_context;

	SharedData* sd = new SharedData(this);

	std::shared_ptr<GSRasterizerData> data(sd);

	sd->primclass = m_vt.m_primclass;
	sd->buff = (uint8*)_aligned_malloc(sizeof(GSVertexSW) * ((m_vertex.next + 1) & ~1) + sizeof(uint32) * m_index.tail, 64);
	sd->vertex = (GSVertexSW*)sd->buff;
	sd->vertex_count = m_vertex.next;
	sd->index = (uint32*)(sd->buff + sizeof(GSVertexSW) * ((m_vertex.next + 1) & ~1));
	sd->index_count = m_index.tail;

	// skip per pixel division if q is constant.
	// Optimize the division by 1 with a nop. It also means that GS_SPRITE_CLASS must be processed when !m_vt.m_eq.q.
	// If you have both GS_SPRITE_CLASS && m_vt.m_eq.q, it will depends on the first part of the 'OR'
	uint32 q_div = !IsMipMapActive() && ((m_vt.m_eq.q && m_vt.m_min.t.z != 1.0f) || (!m_vt.m_eq.q && m_vt.m_primclass == GS_SPRITE_CLASS));

	(this->*m_cvb[m_vt.m_primclass][PRIM->TME][PRIM->FST][q_div])(sd->vertex, m_vertex.buff, m_vertex.next);

	memcpy(sd->index, m_index.buff, sizeof(uint32) * m_index.tail);

	GSVector4i scissor = GSVector4i(context->scissor.in);
	GSVector4i bbox = GSVector4i(m_vt.m_min.p.floor().xyxy(m_vt.m_max.p.ceil()));

	// points and lines may have zero area bbox (single line: 0, 0 - 256, 0)

	if (m_vt.m_primclass == GS_POINT_CLASS || m_vt.m_primclass == GS_LINE_CLASS)
	{
		if (bbox.x == bbox.z) bbox.z++;
		if (bbox.y == bbox.w) bbox.w++;
	}

	GSVector4i r = bbox.rintersect(scissor);

	scissor.z = std::min<int>(scissor.z, (int)context->FRAME.FBW * 64); // TODO: find a game that overflows and check which one is the right behaviour

	sd->scissor = scissor;
	sd->bbox = bbox;
	sd->frame = m_perfmon.GetFrame();

	if (!GetScanlineGlobalData(sd))
	{
		return;
	}

	if (0) if (LOG)
	{
		int n = GSUtil::GetVertexCount(PRIM->PRIM);

		for (uint32 i = 0, j = 0; i < m_index.tail; i += n, j++)
		{
			for (int k = 0; k < n; k++)
			{
				GSVertex* v = &m_vertex.buff[m_index.buff[i + k]];
				GSVertex* vn = &m_vertex.buff[m_index.buff[i + n - 1]];

				fprintf(s_fp, "%d:%d %f %f %f %f\n",
					j, k,
					(float)(v->XYZ.X - context->XYOFFSET.OFX) / 16,
					(float)(v->XYZ.Y - context->XYOFFSET.OFY) / 16,
					PRIM->FST ? (float)(v->U) / 16 : v->ST.S / (PRIM->PRIM == GS_SPRITE ? vn->RGBAQ.Q : v->RGBAQ.Q),
					PRIM->FST ? (float)(v->V) / 16 : v->ST.T / (PRIM->PRIM == GS_SPRITE ? vn->RGBAQ.Q : v->RGBAQ.Q));
			}
		}
	}

	//

	// GSScanlineGlobalData& gd = sd->global;

	GSOffset::PageLooper* fb_pages = NULL;
	GSOffset::PageLooper* zb_pages = NULL;
	GSOffset::PageLooper _fb_pages, _zb_pages;

	if (sd->global.sel.fb)
	{
		_fb_pages = m_context->offset.fb.pageLooperForRect(r);
		fb_pages = &_fb_pages;
	}

	if (sd->global.sel.zb)
	{
		_zb_pages = m_context->offset.zb.pageLooperForRect(r);
		zb_pages = &_zb_pages;
	}

	// check if there is an overlap between this and previous targets

	if (CheckTargetPages(fb_pages, zb_pages, r))
	{
		sd->m_syncpoint = SharedData::SyncTarget;
	}

	// check if the texture is not part of a target currently in use

	if (CheckSourcePages(sd))
	{
		sd->m_syncpoint = SharedData::SyncSource;
	}

	// addref source and target pages

	sd->UsePages(fb_pages, m_context->offset.fb.psm(), zb_pages, m_context->offset.zb.psm());

	//

	if (s_dump)
	{
		Sync(2);

		uint64 frame = m_perfmon.GetFrame();
		// Dump the texture in 32 bits format. It helps to debug texture shuffle effect
		// It will breaks the few games that really uses 16 bits RT
		bool texture_shuffle = ((context->FRAME.PSM & 0x2) && ((context->TEX0.PSM & 3) == 2) && (m_vt.m_primclass == GS_SPRITE_CLASS));

		std::string s;

		if (s_n >= s_saven)
		{
			// Dump Register state
			s = format("%05d_context.txt", s_n);

			m_env.Dump(m_dump_root + s);
			m_context->Dump(m_dump_root + s);
		}

		if (s_savet && s_n >= s_saven && PRIM->TME)
		{
			if (texture_shuffle)
			{
				// Dump the RT in 32 bits format. It helps to debug texture shuffle effect
				s = format("%05d_f%lld_itexraw_%05x_32bits.bmp", s_n, frame, (int)m_context->TEX0.TBP0);
				m_mem.SaveBMP(m_dump_root + s, m_context->TEX0.TBP0, m_context->TEX0.TBW, 0, 1 << m_context->TEX0.TW, 1 << m_context->TEX0.TH);
			}

			s = format("%05d_f%lld_itexraw_%05x_%s.bmp", s_n, frame, (int)m_context->TEX0.TBP0, psm_str(m_context->TEX0.PSM));
			m_mem.SaveBMP(m_dump_root + s, m_context->TEX0.TBP0, m_context->TEX0.TBW, m_context->TEX0.PSM, 1 << m_context->TEX0.TW, 1 << m_context->TEX0.TH);
		}

		if (s_save && s_n >= s_saven)
		{

			if (texture_shuffle)
			{
				// Dump the RT in 32 bits format. It helps to debug texture shuffle effect
				s = format("%05d_f%lld_rt0_%05x_32bits.bmp", s_n, frame, m_context->FRAME.Block());
				m_mem.SaveBMP(m_dump_root + s, m_context->FRAME.Block(), m_context->FRAME.FBW, 0, GetFrameRect().width(), 512);
			}

			s = format("%05d_f%lld_rt0_%05x_%s.bmp", s_n, frame, m_context->FRAME.Block(), psm_str(m_context->FRAME.PSM));
			m_mem.SaveBMP(m_dump_root + s, m_context->FRAME.Block(), m_context->FRAME.FBW, m_context->FRAME.PSM, GetFrameRect().width(), 512);
		}

		if (s_savez && s_n >= s_saven)
		{
			s = format("%05d_f%lld_rz0_%05x_%s.bmp", s_n, frame, m_context->ZBUF.Block(), psm_str(m_context->ZBUF.PSM));

			m_mem.SaveBMP(m_dump_root + s, m_context->ZBUF.Block(), m_context->FRAME.FBW, m_context->ZBUF.PSM, GetFrameRect().width(), 512);
		}

		Queue(data);

		Sync(3);

		if (s_save && s_n >= s_saven)
		{
			if (texture_shuffle)
			{
				// Dump the RT in 32 bits format. It helps to debug texture shuffle effect
				s = format("%05d_f%lld_rt1_%05x_32bits.bmp", s_n, frame, m_context->FRAME.Block());
				m_mem.SaveBMP(m_dump_root + s, m_context->FRAME.Block(), m_context->FRAME.FBW, 0, GetFrameRect().width(), 512);
			}

			s = format("%05d_f%lld_rt1_%05x_%s.bmp", s_n, frame, m_context->FRAME.Block(), psm_str(m_context->FRAME.PSM));
			m_mem.SaveBMP(m_dump_root + s, m_context->FRAME.Block(), m_context->FRAME.FBW, m_context->FRAME.PSM, GetFrameRect().width(), 512);
		}

		if (s_savez && s_n >= s_saven)
		{
			s = format("%05d_f%lld_rz1_%05x_%s.bmp", s_n, frame, m_context->ZBUF.Block(), psm_str(m_context->ZBUF.PSM));

			m_mem.SaveBMP(m_dump_root + s, m_context->ZBUF.Block(), m_context->FRAME.FBW, m_context->ZBUF.PSM, GetFrameRect().width(), 512);
		}

		if (s_savel > 0 && (s_n - s_saven) > s_savel)
		{
			s_dump = 0;
		}
	}
	else
	{
		Queue(data);
	}

	/*
	if(0)//stats.ticks > 5000000)
	{
		printf("* [%lld | %012llx] ticks %lld prims %d (%d) pixels %d (%d)\n",
			m_perfmon.GetFrame(), gd->sel.key,
			stats.ticks,
			stats.prims, stats.prims > 0 ? (int)(stats.ticks / stats.prims) : -1,
			stats.pixels, stats.pixels > 0 ? (int)(stats.ticks / stats.pixels) : -1);
	}
	*/
}

void GSRendererSW::Queue(std::shared_ptr<GSRasterizerData>& item)
{
	SharedData* sd = (SharedData*)item.get();

	if (sd->m_syncpoint == SharedData::SyncSource)
	{
		Sync(4);
	}

	// update previously invalidated parts

	sd->UpdateSource();

	if (sd->m_syncpoint == SharedData::SyncTarget)
	{
		Sync(5);
	}

	if (LOG)
	{
		GSScanlineGlobalData& gd = ((SharedData*)item.get())->global;

		fprintf(s_fp, "[%d] queue %05x %d (%d) %05x %d (%d) %05x %d %dx%d (%d %d %d) | %u %d %d\n",
			sd->counter,
			m_context->FRAME.Block(), m_context->FRAME.PSM, gd.sel.fwrite,
			m_context->ZBUF.Block(), m_context->ZBUF.PSM, gd.sel.zwrite,
			PRIM->TME ? m_context->TEX0.TBP0 : 0xfffff, m_context->TEX0.PSM, (int)m_context->TEX0.TW, (int)m_context->TEX0.TH, m_context->TEX0.CSM, m_context->TEX0.CPSM, m_context->TEX0.CSA,
			PRIM->PRIM, sd->vertex_count, sd->index_count);

		fflush(s_fp);
	}

	m_rl->Queue(item);

	// invalidate new parts rendered onto

	if (sd->global.sel.fwrite)
	{
		m_tc->InvalidatePages(sd->m_fb_pages, sd->m_fpsm);

		m_mem.m_clut.Invalidate(m_context->FRAME.Block());
	}

	if (sd->global.sel.zwrite)
	{
		m_tc->InvalidatePages(sd->m_zb_pages, sd->m_zpsm);
	}
}

void GSRendererSW::Sync(int reason)
{
	//printf("sync %d\n", reason);

	GSPerfMonAutoTimer pmat(&m_perfmon, GSPerfMon::Sync);

	uint64 t = __rdtsc();

	m_rl->Sync();

	if (0) if (LOG)
	{
		std::string s;

		if (s_save)
		{
			s = format("%05d_f%lld_rt1_%05x_%s.bmp", s_n, m_perfmon.GetFrame(), m_context->FRAME.Block(), psm_str(m_context->FRAME.PSM));

			m_mem.SaveBMP(m_dump_root + s, m_context->FRAME.Block(), m_context->FRAME.FBW, m_context->FRAME.PSM, GetFrameRect().width(), 512);
		}

		if (s_savez)
		{
			s = format("%05d_f%lld_zb1_%05x_%s.bmp", s_n, m_perfmon.GetFrame(), m_context->ZBUF.Block(), psm_str(m_context->ZBUF.PSM));

			m_mem.SaveBMP(m_dump_root + s, m_context->ZBUF.Block(), m_context->FRAME.FBW, m_context->ZBUF.PSM, GetFrameRect().width(), 512);
		}
	}

	t = __rdtsc() - t;

	int pixels = m_rl->GetPixels();

	if (LOG)
	{
		fprintf(s_fp, "sync n=%d r=%d t=%llu p=%d %c\n", s_n, reason, t, pixels, t > 10000000 ? '*' : ' ');
		fflush(s_fp);
	}

	m_perfmon.Put(GSPerfMon::Fillrate, pixels);
}

void GSRendererSW::InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r)
{
	if (LOG)
	{
		fprintf(s_fp, "w %05x %u %u, %d %d %d %d\n", BITBLTBUF.DBP, BITBLTBUF.DBW, BITBLTBUF.DPSM, r.x, r.y, r.z, r.w);
		fflush(s_fp);
	}

	GSOffset off = m_mem.GetOffset(BITBLTBUF.DBP, BITBLTBUF.DBW, BITBLTBUF.DPSM);
	GSOffset::PageLooper pages = off.pageLooperForRect(r);

	// check if the changing pages either used as a texture or a target

	if (!m_rl->IsSynced())
	{
		pages.loopPagesWithBreak([&](uint32 page)
		{
			if (m_fzb_pages[page] | m_tex_pages[page])
			{
				Sync(6);

				return false;
			}
			return true;
		});
	}

	m_tc->InvalidatePages(pages, off.psm()); // if texture update runs on a thread and Sync(5) happens then this must come later
}

void GSRendererSW::InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut)
{
	if (LOG)
	{
		fprintf(s_fp, "%s %05x %u %u, %d %d %d %d\n", clut ? "rp" : "r", BITBLTBUF.SBP, BITBLTBUF.SBW, BITBLTBUF.SPSM, r.x, r.y, r.z, r.w);
		fflush(s_fp);
	}

	if (!m_rl->IsSynced())
	{
		GSOffset off = m_mem.GetOffset(BITBLTBUF.SBP, BITBLTBUF.SBW, BITBLTBUF.SPSM);
		GSOffset::PageLooper pages = off.pageLooperForRect(r);

		pages.loopPagesWithBreak([&](uint32 page)
		{
			if (m_fzb_pages[page])
			{
				Sync(7);

				return false;
			}
			return true;
		});
	}
}

void GSRendererSW::UsePages(const GSOffset::PageLooper& pages, const int type)
{
	pages.loopPages([=](uint32 page)
	{
		switch (type)
		{
			case 0:
				ASSERT((m_fzb_pages[page] & 0xFFFF) < USHRT_MAX);
				m_fzb_pages[page] += 1;
				break;
			case 1:
				ASSERT((m_fzb_pages[page] >> 16) < USHRT_MAX);
				m_fzb_pages[page] += 0x10000;
				break;
			case 2:
				ASSERT(m_tex_pages[page] < USHRT_MAX);
				m_tex_pages[page] += 1;
				break;
			default:
				break;
		}
	});
}

void GSRendererSW::ReleasePages(const GSOffset::PageLooper& pages, const int type)
{
	pages.loopPages([=](uint32 page)
	{
		switch (type)
		{
			case 0:
				ASSERT((m_fzb_pages[page] & 0xFFFF) > 0);
				m_fzb_pages[page] -= 1;
				break;
			case 1:
				ASSERT((m_fzb_pages[page] >> 16) > 0);
				m_fzb_pages[page] -= 0x10000;
				break;
			case 2:
				ASSERT(m_tex_pages[page] > 0);
				m_tex_pages[page] -= 1;
				break;
			default:
				break;
		}
	});
}

bool GSRendererSW::CheckTargetPages(const GSOffset::PageLooper* fb_pages, const GSOffset::PageLooper* zb_pages, const GSVector4i& r)
{
	bool synced = m_rl->IsSynced();

	bool fb = fb_pages != NULL;
	bool zb = zb_pages != NULL;

	GSOffset::PageLooper _fb_pages, _zb_pages;
	auto requirePages = [&]
	{
		if (fb_pages == NULL)
		{
			_fb_pages = m_context->offset.fb.pageLooperForRect(r);
			fb_pages = &_fb_pages;
		}
		if (zb_pages == NULL)
		{
			_zb_pages = m_context->offset.zb.pageLooperForRect(r);
			zb_pages = &_zb_pages;
		}
	};

	bool res = false;

	if (m_fzb != m_context->offset.fzb4)
	{
		// targets changed, check everything

		m_fzb = m_context->offset.fzb4;
		m_fzb_bbox = r;

		memset(m_fzb_cur_pages, 0, sizeof(m_fzb_cur_pages));

		uint32 used = 0;

		requirePages();

		fb_pages->loopPages([&](uint32 i)
		{
			uint32 row = i >> 5;
			uint32 col = 1 << (i & 31);

			m_fzb_cur_pages[row] |= col;

			used |= m_fzb_pages[i];
			used |= m_tex_pages[i];
		});

		zb_pages->loopPages([&](uint32 i)
		{
			uint32 row = i >> 5;
			uint32 col = 1 << (i & 31);

			m_fzb_cur_pages[row] |= col;

			used |= m_fzb_pages[i];
			used |= m_tex_pages[i];
		});

		if (!synced)
		{
			if (used)
			{
				if (LOG)
				{
					fprintf(s_fp, "syncpoint 0\n");
					fflush(s_fp);
				}

				res = true;
			}

			//if(LOG) {fprintf(s_fp, "no syncpoint *\n"); fflush(s_fp);}
		}
	}
	else
	{
		// same target, only check new areas and cross-rendering between frame and z-buffer

		GSVector4i bbox = m_fzb_bbox.runion(r);

		bool check = !m_fzb_bbox.eq(bbox);

		m_fzb_bbox = bbox;

		if (check)
		{
			// drawing area is larger than previous time, check new parts only to avoid false positives (m_fzb_cur_pages guards)

			requirePages();

			uint32 used = 0;

			fb_pages->loopPages([&](uint32 i)
			{
				uint32 row = i >> 5;
				uint32 col = 1 << (i & 31);

				if ((m_fzb_cur_pages[row] & col) == 0)
				{
					m_fzb_cur_pages[row] |= col;

					used |= m_fzb_pages[i];
				}
			});

			zb_pages->loopPages([&](uint32 i)
			{
				uint32 row = i >> 5;
				uint32 col = 1 << (i & 31);

				if ((m_fzb_cur_pages[row] & col) == 0)
				{
					m_fzb_cur_pages[row] |= col;

					used |= m_fzb_pages[i];
				}
			});

			if (!synced)
			{
				if (used)
				{
					if (LOG)
					{
						fprintf(s_fp, "syncpoint 1\n");
						fflush(s_fp);
					}

					res = true;
				}
			}
		}

		if (!synced)
		{
			// chross-check frame and z-buffer pages, they cannot overlap with eachother and with previous batches in queue,
			// have to be careful when the two buffers are mutually enabled/disabled and alternating (Bully FBP/ZBP = 0x2300)

			if (fb && !res)
			{
				fb_pages->loopPagesWithBreak([&](uint32 page)
				{
					if (m_fzb_pages[page] & 0xffff0000)
					{
						if (LOG)
						{
							fprintf(s_fp, "syncpoint 2\n");
							fflush(s_fp);
						}

						res = true;

						return false;
					}
					return true;
				});
			}

			if (zb && !res)
			{
				zb_pages->loopPagesWithBreak([&](uint32 page)
				{
					if (m_fzb_pages[page] & 0x0000ffff)
					{
						if (LOG)
						{
							fprintf(s_fp, "syncpoint 3\n");
							fflush(s_fp);
						}

						res = true;

						return false;
					}
					return true;
				});
			}
		}
	}

	return res;
}

bool GSRendererSW::CheckSourcePages(SharedData* sd)
{
	if (!m_rl->IsSynced())
	{
		for (size_t i = 0; sd->m_tex[i].t != NULL; i++)
		{
			GSOffset::PageLooper pages = sd->m_tex[i].t->m_offset.pageLooperForRect(sd->m_tex[i].r);

			bool ret = false;
			pages.loopPagesWithBreak([&](uint32 pages)
			{
				// TODO: 8H 4HL 4HH texture at the same place as the render target (24 bit, or 32-bit where the alpha channel is masked, Valkyrie Profile 2)

				if (m_fzb_pages[pages]) // currently being drawn to? => sync
				{
					ret = true;
					return false;
				}
				return true;
			});
			if (ret)
				return true;
		}
	}

	return false;
}

#include "GSTextureSW.h"

bool GSRendererSW::GetScanlineGlobalData(SharedData* data)
{
	GSScanlineGlobalData& gd = data->global;

	const GSDrawingEnvironment& env = m_env;
	const GSDrawingContext* context = m_context;
	const GS_PRIM_CLASS primclass = m_vt.m_primclass;

	gd.vm = m_mem.m_vm8;

	gd.fbo = context->offset.fb;
	gd.zbo = context->offset.zb;
	gd.fzbr = context->offset.fzb4->row;
	gd.fzbc = context->offset.fzb4->col;

	gd.sel.key = 0;

	gd.sel.fpsm = 3;
	gd.sel.zpsm = 3;
	gd.sel.atst = ATST_ALWAYS;
	gd.sel.tfx = TFX_NONE;
	gd.sel.ababcd = 0xff;
	gd.sel.prim = primclass;

	uint32 fm = context->FRAME.FBMSK;
	uint32 zm = context->ZBUF.ZMSK || context->TEST.ZTE == 0 ? 0xffffffff : 0;

	if (context->TEST.ZTE && context->TEST.ZTST == ZTST_NEVER)
	{
		fm = 0xffffffff;
		zm = 0xffffffff;
	}

	if (PRIM->TME)
	{
		if (GSLocalMemory::m_psm[context->TEX0.PSM].pal > 0)
		{
			m_mem.m_clut.Read32(context->TEX0, env.TEXA);
		}
	}

	if (context->TEST.ATE)
	{
		if (!TryAlphaTest(fm, zm))
		{
			gd.sel.atst = context->TEST.ATST;
			gd.sel.afail = context->TEST.AFAIL;

			gd.aref = GSVector4i((int)context->TEST.AREF);

			switch (gd.sel.atst)
			{
				case ATST_LESS:
					gd.sel.atst = ATST_LEQUAL;
					gd.aref -= GSVector4i::x00000001();
					break;
				case ATST_GREATER:
					gd.sel.atst = ATST_GEQUAL;
					gd.aref += GSVector4i::x00000001();
					break;
			}
		}
	}

	bool fwrite = fm != 0xffffffff;
	bool ftest = gd.sel.atst != ATST_ALWAYS || context->TEST.DATE && context->FRAME.PSM != PSM_PSMCT24;

	bool zwrite = zm != 0xffffffff;
	bool ztest = context->TEST.ZTE && context->TEST.ZTST > ZTST_ALWAYS;
	/*
	printf("%05x %d %05x %d %05x %d %dx%d\n", 
		fwrite || ftest ? m_context->FRAME.Block() : 0xfffff, m_context->FRAME.PSM,
		zwrite || ztest ? m_context->ZBUF.Block() : 0xfffff, m_context->ZBUF.PSM,
		PRIM->TME ? m_context->TEX0.TBP0 : 0xfffff, m_context->TEX0.PSM, (int)m_context->TEX0.TW, (int)m_context->TEX0.TH);
	*/
	if (!fwrite && !zwrite)
		return false;

	gd.sel.fwrite = fwrite;
	gd.sel.ftest = ftest;

	if (fwrite || ftest)
	{
		gd.sel.fpsm = GSLocalMemory::m_psm[context->FRAME.PSM].fmt;

		if ((primclass == GS_LINE_CLASS || primclass == GS_TRIANGLE_CLASS) && m_vt.m_eq.rgba != 0xffff)
		{
			gd.sel.iip = PRIM->IIP;
		}

		if (PRIM->TME)
		{
			gd.sel.tfx = context->TEX0.TFX;
			gd.sel.tcc = context->TEX0.TCC;
			gd.sel.fst = PRIM->FST;
			gd.sel.ltf = m_vt.IsLinear();

			if (GSLocalMemory::m_psm[context->TEX0.PSM].pal > 0)
			{
				gd.sel.tlu = 1;

				gd.clut = (uint32*)_aligned_malloc(sizeof(uint32) * 256, 32); // FIXME: might address uninitialized data of the texture (0xCD) that is not in 0-15 range for 4-bpp formats

				memcpy(gd.clut, (const uint32*)m_mem.m_clut, sizeof(uint32) * GSLocalMemory::m_psm[context->TEX0.PSM].pal);
			}

			gd.sel.wms = context->CLAMP.WMS;
			gd.sel.wmt = context->CLAMP.WMT;

			if (gd.sel.tfx == TFX_MODULATE && gd.sel.tcc && m_vt.m_eq.rgba == 0xffff && m_vt.m_min.c.eq(GSVector4i(128)))
			{
				// modulate does not do anything when vertex color is 0x80

				gd.sel.tfx = TFX_DECAL;
			}

			bool mipmap = IsMipMapActive();

			GIFRegTEX0 TEX0 = m_context->GetSizeFixedTEX0(m_vt.m_min.t.xyxy(m_vt.m_max.t), m_vt.IsLinear(), mipmap);

			GSVector4i r;

			GetTextureMinMax(r, TEX0, context->CLAMP, gd.sel.ltf);

			GSTextureCacheSW::Texture* t = m_tc->Lookup(TEX0, env.TEXA);

			if (t == NULL)
			{
				ASSERT(0);
				return false;
			}

			data->SetSource(t, r, 0);

			gd.sel.tw = t->m_tw - 3;

			if (mipmap)
			{
				// TEX1.MMIN
				// 000 p
				// 001 l
				// 010 p round
				// 011 p tri
				// 100 l round
				// 101 l tri

				if (m_vt.m_lod.x > 0)
				{
					gd.sel.ltf = context->TEX1.MMIN >> 2;
				}
				else
				{
					// TODO: isbilinear(mmag) != isbilinear(mmin) && m_vt.m_lod.x <= 0 && m_vt.m_lod.y > 0
				}

				gd.sel.mmin = (context->TEX1.MMIN & 1) + 1; // 1: round, 2: tri
				gd.sel.lcm = context->TEX1.LCM;

				int mxl = std::min<int>((int)context->TEX1.MXL, 6) << 16;
				int k = context->TEX1.K << 12;

				if ((int)m_vt.m_lod.x >= (int)context->TEX1.MXL)
				{
					k = (int)m_vt.m_lod.x << 16; // set lod to max level

					gd.sel.lcm = 1;  // lod is constant
					gd.sel.mmin = 1; // tri-linear is meaningless
				}

				if (gd.sel.mmin == 2)
				{
					mxl--; // don't sample beyond the last level (TODO: add a dummy level instead?)
				}

				if (gd.sel.fst)
				{
					ASSERT(gd.sel.lcm == 1);
					ASSERT(((m_vt.m_min.t.uph(m_vt.m_max.t) == GSVector4::zero()).mask() & 3) == 3); // ratchet and clank (menu)

					gd.sel.lcm = 1;
				}

				if (gd.sel.lcm)
				{
					int lod = std::max<int>(std::min<int>(k, mxl), 0);

					if (gd.sel.mmin == 1)
					{
						lod = (lod + 0x8000) & 0xffff0000; // rounding
					}

					gd.lod.i = GSVector4i(lod >> 16);
					gd.lod.f = GSVector4i(lod & 0xffff).xxxxl().xxzz();

					// TODO: lot to optimize when lod is constant
				}
				else
				{
					gd.mxl = GSVector4((float)mxl);
					gd.l = GSVector4((float)(-(0x10000 << context->TEX1.L)));
					gd.k = GSVector4((float)k);
				}

				GIFRegCLAMP MIP_CLAMP = context->CLAMP;

				GSVector4 tmin = m_vt.m_min.t;
				GSVector4 tmax = m_vt.m_max.t;

				static int s_counter = 0;

				for (int i = 1, j = std::min<int>((int)context->TEX1.MXL, 6); i <= j; i++)
				{
					const GIFRegTEX0& MIP_TEX0 = GetTex0Layer(i);

					MIP_CLAMP.MINU >>= 1;
					MIP_CLAMP.MINV >>= 1;
					MIP_CLAMP.MAXU >>= 1;
					MIP_CLAMP.MAXV >>= 1;

					m_vt.m_min.t *= 0.5f;
					m_vt.m_max.t *= 0.5f;

					GSTextureCacheSW::Texture* t = m_tc->Lookup(MIP_TEX0, env.TEXA, gd.sel.tw + 3);

					if (t == NULL)
					{
						ASSERT(0);
						return false;
					}

					GSVector4i r;

					GetTextureMinMax(r, MIP_TEX0, MIP_CLAMP, gd.sel.ltf);

					data->SetSource(t, r, i);
				}

				s_counter++;

				m_vt.m_min.t = tmin;
				m_vt.m_max.t = tmax;
			}
			else
			{
				// skip per pixel division if q is constant. Sprite uses flat
				// q, so it's always constant by primitive.
				// Note: the 'q' division was done in GSRendererSW::ConvertVertexBuffer
				gd.sel.fst |= (m_vt.m_eq.q || primclass == GS_SPRITE_CLASS);

				if (gd.sel.ltf && gd.sel.fst)
				{
					// if q is constant we can do the half pel shift for bilinear sampling on the vertices

					// TODO: but not when mipmapping is used!!!

					GSVector4 half(0x8000, 0x8000);

					GSVertexSW* RESTRICT v = data->vertex;

					for (int i = 0, j = data->vertex_count; i < j; i++)
					{
						GSVector4 t = v[i].t;

						v[i].t = (t - half).xyzw(t);
					}
				}
			}

			uint16 tw = 1u << TEX0.TW;
			uint16 th = 1u << TEX0.TH;

			switch (context->CLAMP.WMS)
			{
				case CLAMP_REPEAT:
					gd.t.min.u16[0] = gd.t.minmax.u16[0] = tw - 1;
					gd.t.max.u16[0] = gd.t.minmax.u16[2] = 0;
					gd.t.mask.u32[0] = 0xffffffff;
					break;
				case CLAMP_CLAMP:
					gd.t.min.u16[0] = gd.t.minmax.u16[0] = 0;
					gd.t.max.u16[0] = gd.t.minmax.u16[2] = tw - 1;
					gd.t.mask.u32[0] = 0;
					break;
				case CLAMP_REGION_CLAMP:
					gd.t.min.u16[0] = gd.t.minmax.u16[0] = std::min<uint16>(context->CLAMP.MINU, tw - 1);
					gd.t.max.u16[0] = gd.t.minmax.u16[2] = std::min<uint16>(context->CLAMP.MAXU, tw - 1);
					gd.t.mask.u32[0] = 0;
					break;
				case CLAMP_REGION_REPEAT:
					gd.t.min.u16[0] = gd.t.minmax.u16[0] = context->CLAMP.MINU & (tw - 1);
					gd.t.max.u16[0] = gd.t.minmax.u16[2] = context->CLAMP.MAXU & (tw - 1);
					gd.t.mask.u32[0] = 0xffffffff;
					break;
				default:
					__assume(0);
			}

			switch (context->CLAMP.WMT)
			{
				case CLAMP_REPEAT:
					gd.t.min.u16[4] = gd.t.minmax.u16[1] = th - 1;
					gd.t.max.u16[4] = gd.t.minmax.u16[3] = 0;
					gd.t.mask.u32[2] = 0xffffffff;
					break;
				case CLAMP_CLAMP:
					gd.t.min.u16[4] = gd.t.minmax.u16[1] = 0;
					gd.t.max.u16[4] = gd.t.minmax.u16[3] = th - 1;
					gd.t.mask.u32[2] = 0;
					break;
				case CLAMP_REGION_CLAMP:
					gd.t.min.u16[4] = gd.t.minmax.u16[1] = std::min<uint16>(context->CLAMP.MINV, th - 1);
					gd.t.max.u16[4] = gd.t.minmax.u16[3] = std::min<uint16>(context->CLAMP.MAXV, th - 1); // ffx anima summon scene, when the anchor appears (th = 256, maxv > 256)
					gd.t.mask.u32[2] = 0;
					break;
				case CLAMP_REGION_REPEAT:
					gd.t.min.u16[4] = gd.t.minmax.u16[1] = context->CLAMP.MINV & (th - 1); // skygunner main menu water texture 64x64, MINV = 127
					gd.t.max.u16[4] = gd.t.minmax.u16[3] = context->CLAMP.MAXV & (th - 1);
					gd.t.mask.u32[2] = 0xffffffff;
					break;
				default:
					__assume(0);
			}

			gd.t.min = gd.t.min.xxxxlh();
			gd.t.max = gd.t.max.xxxxlh();
			gd.t.mask = gd.t.mask.xxzz();
			gd.t.invmask = ~gd.t.mask;
		}

		if (PRIM->FGE)
		{
			gd.sel.fge = 1;

			gd.frb = env.FOGCOL.u32[0] & 0x00ff00ff;
			gd.fga = (env.FOGCOL.u32[0] >> 8) & 0x00ff00ff;
		}

		if (context->FRAME.PSM != PSM_PSMCT24)
		{
			gd.sel.date = context->TEST.DATE;
			gd.sel.datm = context->TEST.DATM;
		}

		if (!IsOpaque())
		{
			gd.sel.abe = PRIM->ABE;
			gd.sel.ababcd = context->ALPHA.u32[0];

			if (env.PABE.PABE)
			{
				gd.sel.pabe = 1;
			}

			if (m_aa1 && PRIM->AA1 && (primclass == GS_LINE_CLASS || primclass == GS_TRIANGLE_CLASS))
			{
				gd.sel.aa1 = 1;
			}

			gd.afix = GSVector4i((int)context->ALPHA.FIX << 7).xxzzlh();
		}

		if (gd.sel.date
		 || gd.sel.aba == 1 || gd.sel.abb == 1 || gd.sel.abc == 1 || gd.sel.abd == 1
		 || gd.sel.atst != ATST_ALWAYS && gd.sel.afail == AFAIL_RGB_ONLY
		 || gd.sel.fpsm == 0 && fm != 0 && fm != 0xffffffff
		 || gd.sel.fpsm == 1 && (fm & 0x00ffffff) != 0 && (fm & 0x00ffffff) != 0x00ffffff
		 || gd.sel.fpsm == 2 && (fm & 0x80f8f8f8) != 0 && (fm & 0x80f8f8f8) != 0x80f8f8f8)
		{
			gd.sel.rfb = 1;
		}

		gd.sel.colclamp = env.COLCLAMP.CLAMP;
		gd.sel.fba = context->FBA.FBA;

		if (env.DTHE.DTHE)
		{
			gd.sel.dthe = 1;

			gd.dimx = (GSVector4i*)_aligned_malloc(sizeof(env.dimx), 32);

			memcpy(gd.dimx, env.dimx, sizeof(env.dimx));
		}
	}

	gd.sel.zwrite = zwrite;
	gd.sel.ztest = ztest;

	if (zwrite || ztest)
	{
		uint32_t z_max = 0xffffffff >> (GSLocalMemory::m_psm[context->ZBUF.PSM].fmt * 8);

		gd.sel.zpsm = GSLocalMemory::m_psm[context->ZBUF.PSM].fmt;
		gd.sel.ztst = ztest ? context->TEST.ZTST : (int)ZTST_ALWAYS;
		gd.sel.zoverflow = (uint32)GSVector4i(m_vt.m_max.p).z == 0x80000000U;
		gd.sel.zclamp = (uint32)GSVector4i(m_vt.m_max.p).z > z_max;
	}

#if _M_SSE >= 0x501

	gd.fm = fm;
	gd.zm = zm;

	if (gd.sel.fpsm == 1)
	{
		gd.fm |= 0xff000000;
	}
	else if (gd.sel.fpsm == 2)
	{
		uint32 rb = gd.fm & 0x00f800f8;
		uint32 ga = gd.fm & 0x8000f800;

		gd.fm = (ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3) | 0xffff0000;
	}

	if (gd.sel.zpsm == 1)
	{
		gd.zm |= 0xff000000;
	}
	else if (gd.sel.zpsm == 2)
	{
		gd.zm |= 0xffff0000;
	}

#else

	gd.fm = GSVector4i(fm);
	gd.zm = GSVector4i(zm);

	if (gd.sel.fpsm == 1)
	{
		gd.fm |= GSVector4i::xff000000();
	}
	else if (gd.sel.fpsm == 2)
	{
		GSVector4i rb = gd.fm & 0x00f800f8;
		GSVector4i ga = gd.fm & 0x8000f800;

		gd.fm = (ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3) | GSVector4i::xffff0000();
	}

	if (gd.sel.zpsm == 1)
	{
		gd.zm |= GSVector4i::xff000000();
	}
	else if (gd.sel.zpsm == 2)
	{
		gd.zm |= GSVector4i::xffff0000();
	}

#endif

	if (gd.sel.prim == GS_SPRITE_CLASS && !gd.sel.ftest && !gd.sel.ztest && data->bbox.eq(data->bbox.rintersect(data->scissor))) // TODO: check scissor horizontally only
	{
		gd.sel.notest = 1;

		uint32 ofx = context->XYOFFSET.OFX;

		for (int i = 0, j = m_vertex.tail; i < j; i++)
		{
#if _M_SSE >= 0x501
			if ((((m_vertex.buff[i].XYZ.X - ofx) + 15) >> 4) & 7) // aligned to 8
#else
			if ((((m_vertex.buff[i].XYZ.X - ofx) + 15) >> 4) & 3) // aligned to 4
#endif
			{
				gd.sel.notest = 0;

				break;
			}
		}
	}

	return true;
}

GSRendererSW::SharedData::SharedData(GSRendererSW* parent)
	: m_parent(parent)
	, m_fpsm(0)
	, m_zpsm(0)
	, m_using_pages(false)
	, m_syncpoint(SyncNone)
{
	m_tex[0].t = NULL;

	global.sel.key = 0;

	global.clut = NULL;
	global.dimx = NULL;
}

GSRendererSW::SharedData::~SharedData()
{
	ReleasePages();

	if (global.clut)
		_aligned_free(global.clut);
	if (global.dimx)
		_aligned_free(global.dimx);

	if (LOG)
	{
		fprintf(s_fp, "[%d] done t=%lld p=%d | %d %d %d | %08x_%08x\n",
			counter,
			__rdtsc() - start, pixels,
			primclass, vertex_count, index_count,
			global.sel.hi, global.sel.lo);
		fflush(s_fp);
	}
}

//static TransactionScope::Lock s_lock;

void GSRendererSW::SharedData::UsePages(const GSOffset::PageLooper* fb_pages, int fpsm, const GSOffset::PageLooper* zb_pages, int zpsm)
{
	if (m_using_pages)
		return;

	{
		//TransactionScope scope(s_lock);

		if (global.sel.fb)
		{
			m_parent->UsePages(*fb_pages, 0);
		}

		if (global.sel.zb)
		{
			m_parent->UsePages(*zb_pages, 1);
		}

		for (size_t i = 0; m_tex[i].t != NULL; i++)
		{
			m_parent->UsePages(m_tex[i].t->m_pages, 2);
		}
	}

	if (fb_pages)
		m_fb_pages = *fb_pages;
	if (zb_pages)
		m_zb_pages = *zb_pages;
	m_fpsm = fpsm;
	m_zpsm = zpsm;

	m_using_pages = true;
}

void GSRendererSW::SharedData::ReleasePages()
{
	if (!m_using_pages)
		return;

	{
		//TransactionScope scope(s_lock);

		if (global.sel.fb)
		{
			m_parent->ReleasePages(m_fb_pages, 0);
		}

		if (global.sel.zb)
		{
			m_parent->ReleasePages(m_zb_pages, 1);
		}

		for (size_t i = 0; m_tex[i].t != NULL; i++)
		{
			m_parent->ReleasePages(m_tex[i].t->m_pages, 2);
		}
	}

	m_using_pages = false;
}

void GSRendererSW::SharedData::SetSource(GSTextureCacheSW::Texture* t, const GSVector4i& r, int level)
{
	ASSERT(m_tex[level].t == NULL);

	m_tex[level].t = t;
	m_tex[level].r = r;

	m_tex[level + 1].t = NULL;
}

void GSRendererSW::SharedData::UpdateSource()
{
	for (size_t i = 0; m_tex[i].t != NULL; i++)
	{
		if (m_tex[i].t->Update(m_tex[i].r))
		{
			global.tex[i] = m_tex[i].t->m_buff;
		}
		else
		{
			printf("GS: out-of-memory, texturing temporarily disabled\n");

			global.sel.tfx = TFX_NONE;
		}
	}

	// TODO

	if (m_parent->s_dump)
	{
		uint64 frame = m_parent->m_perfmon.GetFrame();

		std::string s;

		if (m_parent->s_savet && m_parent->s_n >= m_parent->s_saven)
		{
			for (size_t i = 0; m_tex[i].t != NULL; i++)
			{
				const GIFRegTEX0& TEX0 = m_parent->GetTex0Layer(i);

				s = format("%05d_f%lld_itex%d_%05x_%s.bmp", m_parent->s_n, frame, i, TEX0.TBP0, psm_str(TEX0.PSM));

				m_tex[i].t->Save(root_sw + s);
			}

			if (global.clut != NULL)
			{
				GSTextureSW* t = new GSTextureSW(0, 256, 1);

				t->Update(GSVector4i(0, 0, 256, 1), global.clut, sizeof(uint32) * 256);

				s = format("%05d_f%lld_itexp_%05x_%s.bmp", m_parent->s_n, frame, (int)m_parent->m_context->TEX0.CBP, psm_str(m_parent->m_context->TEX0.CPSM));

				t->Save(root_sw + s);

				delete t;
			}
		}
	}
}
