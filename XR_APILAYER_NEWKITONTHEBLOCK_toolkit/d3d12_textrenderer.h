// MIT License
//
// Copyright(c) 2026 Tsevopolus
// 
// D3D12TextRenderer — a minimal, native D3D12 replacement for the FW1FontWrapper +
// D3D11on12 interop text-rendering path.
//
// FIRST DRAFT — not yet tested on real hardware. See the accompanying notes for known
// simplifications and things to verify once you build & run this in-headset.
//
// Design:
//  - The font atlas is rasterized ONCE at startup using plain GDI (CreateFontW +
//    ExtTextOutW into a DIB section). This avoids hand-authored bitmap-font data (too easy
//    to get subtly wrong without being able to see the result) and avoids DirectWrite's
//    heavier COM object model. GDI's text APIs have been stable since Windows 95.
//  - The atlas covers printable ASCII (32-126), which is all the toolkit's own menu
//    strings use. Non-ASCII characters are rendered as a blank cell (see TODO below on
//    where to extend this if you ever localize the menu).
//  - Both weights (Normal and Bold) are baked into one shared atlas texture, stacked
//    vertically - Normal's rows on top, Bold's directly below, using the same column/cell
//    grid (sized to fit whichever weight's glyphs are larger at a given cell) so a single
//    SRV and root signature still cover both. See bakeFontAtlas().
//  - Rendering is a simple batched textured-quad draw: drawString() appends quads to a
//    CPU-side vector, flushText() uploads them into a per-frame dynamic (upload-heap)
//    vertex buffer and issues one DrawInstanced call per flush.
//
// This class deliberately does NOT implement the full IDevice interface. It is used as a
// plain member of D3D12Device, not as a separate wrapped IDevice — see the integration
// notes for how the four IDevice text methods on D3D12Device should delegate to this.

#pragma once

#include "pch.h"
#include "interfaces.h"

namespace toolkit::graphics::d3d12text {

    // Local copy of d3d12.cpp's own SetDebugName() helper - that one lives in d3d12.cpp's
    // private anonymous namespace and isn't visible here, so we keep a tiny duplicate rather
    // than promoting it to a shared header just for this.
    inline void SetDebugName(ID3D12Object* resource, std::string_view name) {
        if (resource && !name.empty())
            resource->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
    }

    // One glyph's location and metrics within the atlas texture.
    struct GlyphInfo {
        float u0{}, v0{}, u1{}, v1{}; // Normalized atlas UV rectangle.
        float width{}, height{};     // Glyph cell size in pixels (at bake size).
        float advance{};             // Horizontal advance in pixels (at bake size).
    };

    struct TextVertex {
        float x, y;    // Screen-space position in pixels (top-left origin, like FW1).
        float u, v;    // Atlas UV.
        uint32_t color; // Packed 0xAABBGGRR (matches what the pixel shader expects, see HLSL below).
    };

    constexpr wchar_t FirstGlyph = 32;  // ' '
    constexpr wchar_t LastGlyph = 126;  // '~'
    constexpr int GlyphCount = LastGlyph - FirstGlyph + 1;
    constexpr int AtlasColumns = 16;
    constexpr int AtlasRows = (GlyphCount + AtlasColumns - 1) / AtlasColumns;

    // Bake size in pixels. drawString()'s `size` parameter scales relative to this at draw
    // time (see scale computation in drawString below), so this only affects rasterization
    // sharpness, not the range of sizes you can request.
    constexpr int BakePixelHeight = 48;
    constexpr int CellPadding = 6; // Extra margin on top of the font's own real metrics (see bakeFontAtlas()).

    class D3D12TextRenderer {
      public:
        // device/commandQueue: the same D3D12 device/queue the rest of D3D12Device uses.
        //
        // Unlike the first sketch of this class, we do NOT take an RTV format here: at
        // construction time (D3D12Device's own constructor) the actual texture text will be
        // drawn onto isn't known yet - it's whatever texture the caller most recently bound
        // via setRenderTargets() before calling beginText(). The PSO is therefore created
        // lazily on the first beginText() call (see ensurePipeline()), and reused as long as
        // the format doesn't change (it practically never does across a session).
        void initialize(ID3D12Device* device, ID3D12CommandQueue* commandQueue) {
            m_device = device;

            // Order matters here:
            //  1. m_uploadQueue must be set before bakeFontAtlas(), which uploads the atlas
            //     texture through it.
            //  2. createUploadHeapAndDescriptors() (SRV + vertex buffer) must run AFTER
            //     bakeFontAtlas(), because it creates the SRV pointing at m_atlasTexture,
            //     which bakeFontAtlas() is what actually creates.
            m_uploadQueue = commandQueue;
            bakeFontAtlas();
            createRootSignature();
            createUploadHeapAndDescriptors();
            // The PSO itself is deferred to the first beginText() call - see ensurePipeline().
        }

        // --- IDevice-shaped surface, to be called from D3D12Device's own overrides ---

        float measureString(std::wstring_view string, bool bold, float size) const {
            const float scale = size / (float)BakePixelHeight;
            float width = 0.f;
            for (wchar_t ch : string) {
                width += glyphFor(ch, bold).advance * scale;
            }
            return width;
        }

        void beginText(ID3D12GraphicsCommandList* commandList,
                       D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView,
                       DXGI_FORMAT renderTargetFormat,
                       uint32_t renderTargetSampleCount,
                       int32_t viewportX,
                       int32_t viewportY,
                       uint32_t viewportWidth,
                       uint32_t viewportHeight) {
            // renderTargetSampleCount == 0 would produce an invalid (zero-sample) PSO; treat
            // it the same as the common "not multisampled" case.
            ensurePipeline(renderTargetFormat, std::max(renderTargetSampleCount, 1u));

            m_commandList = commandList;
            m_renderTargetView = renderTargetView;
            m_viewportX = viewportX;
            m_viewportY = viewportY;
            m_viewportWidth = viewportWidth;
            m_viewportHeight = viewportHeight;
            m_pendingVertices.clear();
        }

        // Mirrors FW1's drawString() contract: appends quads to the pending batch; nothing
        // hits the GPU until flushText(). Returns the measured width when measure=true,
        // matching the existing IDevice::drawString() semantics.
        float drawString(std::wstring_view string,
                         bool bold,
                         float size,
                         float x,
                         float y,
                         uint32_t color,
                         bool measure,
                         int alignment) {
            const float scale = size / (float)BakePixelHeight;
            const float totalWidth = measureString(string, bold, size);

            float penX = x;
            if (alignment & TextAlignRight) {
                penX -= totalWidth;
            } else if (alignment & TextAlignCenter) {
                penX -= totalWidth * 0.5f;
            }
            // NOTE: vertical alignment (Top/Bottom/VCenter) is not yet implemented - `y` is
            // always treated as the glyph baseline's top, matching TextAlignTop. If the menu
            // relies on FW1_VCENTER/FW1_BOTTOM anywhere, this will need the glyph ascent/
            // descent metrics added to GlyphInfo and applied here.

            float penY = y;

            for (wchar_t ch : string) {
                const GlyphInfo& g = glyphFor(ch, bold);

                // g.width is the FULL baked cell width (tmMaxCharWidth + CellPadding*2 - see
                // bakeFontAtlas()), which is generally wider than this glyph's own advance. Using
                // the full cell as the quad width here would make adjacent glyphs' quads overlap
                // by (cellWidth - advance) pixels; both cells' padding is transparent there, so
                // it's normally invisible, but two overlapping semi-transparent quads still
                // double-blend at the antialiased edges within that overlap, which can show as a
                // faint darkening. Shrink the quad to just this glyph's own advance plus a
                // CellPadding safety margin on each side (enough room for any antialiasing/
                // overhang beyond the advance) instead of the shared, wider cell size, and remap
                // the right UV edge to match - this keeps quads from adjacent glyphs from
                // overlapping in the first place, without needing per-glyph ink bounding boxes.
                const float inkWidth = std::min(g.width, g.advance + 2.f * CellPadding);
                const float gw = inkWidth * scale;
                const float gh = g.height * scale;
                const float u1 = g.u0 + (inkWidth / g.width) * (g.u1 - g.u0);

                appendQuad(penX, penY, gw, gh, g.u0, g.v0, u1, g.v1, color);

                penX += g.advance * scale;
            }

            return measure ? totalWidth : 0.f;
        }

        void flushText() {
            if (m_pendingVertices.empty() || !m_commandList) {
                m_commandList = nullptr;
                return;
            }

            size_t requiredBytes = m_pendingVertices.size() * sizeof(TextVertex);
            if (requiredBytes > m_uploadSlotSize) {
                // This used to CHECK_MSG/throw here. That exception has nothing catching it
                // between here and xrEndFrame() - it would take down the whole process over a
                // single oversized menu screen, rather than just losing this frame's text. Drop
                // the overflow instead: clamp to whole quads (6 verts each) so we never submit a
                // partial/garbled quad, log it so it's visible during development, and draw what
                // fits.
                const size_t maxVerts = (m_uploadSlotSize / sizeof(TextVertex)) / 6 * 6;
                toolkit::log::Log(
                    "Text renderer: too many glyphs in one frame (%zu of %zu budgeted bytes) - "
                    "dropping the overflow instead of growing/crashing\n",
                    requiredBytes,
                    m_uploadSlotSize);
                m_pendingVertices.resize(maxVerts);
                requiredBytes = m_pendingVertices.size() * sizeof(TextVertex);
            }

            // Ping-pong between two halves of the upload buffer. m_vertexUploadBuffer is an
            // upload-heap (CPU-writable) resource that DrawInstanced below reads from directly -
            // with a single buffer, this Map/memcpy/Unmap would race the GPU still reading last
            // flush's draw from the very same bytes, since nothing here waits for that draw to
            // finish (VR overlays are exactly the case where the CPU commonly runs a frame or two
            // ahead of the GPU). Alternating slots means each slot's previous contents are always
            // at least one flush old by the time we overwrite it again.
            m_currentUploadSlot = 1 - m_currentUploadSlot;
            const size_t slotOffset = m_currentUploadSlot * m_uploadSlotSize;

            void* mapped = nullptr;
            const CD3DX12_RANGE noRead(0, 0);
            CHECK_HRCMD(m_vertexUploadBuffer->Map(0, &noRead, &mapped));
            memcpy(static_cast<uint8_t*>(mapped) + slotOffset, m_pendingVertices.data(), requiredBytes);
            const CD3DX12_RANGE writtenRange(slotOffset, slotOffset + requiredBytes);
            m_vertexUploadBuffer->Unmap(0, &writtenRange);

            m_commandList->SetGraphicsRootSignature(get(m_rootSignature));
            m_commandList->SetPipelineState(get(m_pipelineState));

            ID3D12DescriptorHeap* heaps[] = {get(m_srvHeap)};
            m_commandList->SetDescriptorHeaps(1, heaps);
            m_commandList->SetGraphicsRootDescriptorTable(0, m_srvHeap->GetGPUDescriptorHandleForHeapStart());

            // The GPU viewport transform maps our clip-space quads (built below relative to
            // the viewport's own top-left, i.e. pixel (0,0) = viewport top-left) to the
            // correct offset within the actual render target - so RSSetViewports is what
            // positions text correctly on multi-view/atlas render targets, not the vertex
            // shader math.
            const D3D12_VIEWPORT viewport{(float)m_viewportX,
                                          (float)m_viewportY,
                                          (float)m_viewportWidth,
                                          (float)m_viewportHeight,
                                          0.f,
                                          1.f};
            const D3D12_RECT scissor{
                m_viewportX, m_viewportY, m_viewportX + (LONG)m_viewportWidth, m_viewportY + (LONG)m_viewportHeight};
            m_commandList->RSSetViewports(1, &viewport);
            m_commandList->RSSetScissorRects(1, &scissor);

            m_commandList->OMSetRenderTargets(1, &m_renderTargetView, false, nullptr);

            const float invW = 2.f / (float)m_viewportWidth;
            const float invH = 2.f / (float)m_viewportHeight;
            const float screenToClip[2] = {invW, invH}; // See vertex shader below.
            m_commandList->SetGraphicsRoot32BitConstants(1, 2, screenToClip, 0);

            D3D12_VERTEX_BUFFER_VIEW vbv{};
            vbv.BufferLocation = m_vertexUploadBuffer->GetGPUVirtualAddress() + slotOffset;
            vbv.SizeInBytes = (UINT)requiredBytes;
            vbv.StrideInBytes = sizeof(TextVertex);
            m_commandList->IASetVertexBuffers(0, 1, &vbv);
            m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            m_commandList->DrawInstanced((UINT)m_pendingVertices.size(), 1, 0, 0);

            m_pendingVertices.clear();
            m_commandList = nullptr;
        }

      private:
        const GlyphInfo& glyphFor(wchar_t ch, bool bold) const {
            const GlyphInfo* glyphs = bold ? m_glyphsBold : m_glyphs;
            if (ch < FirstGlyph || ch > LastGlyph) {
                return glyphs[0]; // Blank/space cell - see class comment on ASCII-only coverage.
            }
            return glyphs[ch - FirstGlyph];
        }

        void appendQuad(
            float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t color) {
            // Two triangles, top-left origin, matching FW1's screen-space pixel coordinates.
            const TextVertex v[6] = {
                {x, y, u0, v0, color},
                {x + w, y, u1, v0, color},
                {x, y + h, u0, v1, color},
                {x + w, y, u1, v0, color},
                {x + w, y + h, u1, v1, color},
                {x, y + h, u0, v1, color},
            };
            m_pendingVertices.insert(m_pendingVertices.end(), std::begin(v), std::end(v));
        }

        // Rasterizes printable ASCII, in both weights, into a single grayscale atlas using
        // GDI, then uploads it as an R8_UNORM D3D12 texture (sampled and tinted by `color` in
        // the pixel shader). Normal's glyphs occupy the top AtlasRows rows, Bold's occupy an
        // identical block of AtlasRows rows directly below - see the class comment.
        void bakeFontAtlas() {
            HDC screenDC = GetDC(nullptr);
            HDC memDC = CreateCompatibleDC(screenDC);

            struct Weight {
                HFONT font;
                TEXTMETRICW tm{};
                GlyphInfo* glyphs;
            };
            Weight weights[2] = {
                {CreateFontW(-BakePixelHeight,
                             0,
                             0,
                             0,
                             FW_NORMAL,
                             FALSE,
                             FALSE,
                             FALSE,
                             DEFAULT_CHARSET,
                             OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE,
                             L"Segoe UI"),
                 {},
                 m_glyphs},
                {CreateFontW(-BakePixelHeight,
                             0,
                             0,
                             0,
                             FW_BOLD,
                             FALSE,
                             FALSE,
                             FALSE,
                             DEFAULT_CHARSET,
                             OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE,
                             L"Segoe UI"),
                 {},
                 m_glyphsBold},
            };

            // Put a valid font in the DC before anything else touches it, and remember it so we
            // can restore it below before deleting our own font objects (GDI convention: never
            // delete a font object while it's still selected into a DC).
            HGDIOBJ oldFontForMetrics = SelectObject(memDC, weights[0].font);

            // Size cells from the FONTS' OWN real metrics rather than guessing. This matters
            // because CreateFontW's height only controls the em-square - actual glyph ink
            // (especially descenders on g/y/p/q/j) commonly extends well beyond that, and if
            // the cell is too small, one row's descenders bleed into the row below it in the
            // atlas (visible as stray marks above unrelated glyphs). tmHeight already covers
            // ascent+descent; we add generous extra padding on top as a further safety margin,
            // since ANTIALIASED_QUALITY can still spill a pixel or two past the reported
            // metrics. The cell grid is shared between both weights (so a single SRV/atlas
            // layout works for both), sized to whichever weight needs more room - Bold is
            // usually a little wider than Normal at the same point size.
            for (auto& w : weights) {
                SelectObject(memDC, w.font);
                GetTextMetricsW(memDC, &w.tm);
            }
            const int cellW = std::max(weights[0].tm.tmMaxCharWidth, weights[1].tm.tmMaxCharWidth) + CellPadding * 2;
            const int cellH = std::max(weights[0].tm.tmHeight, weights[1].tm.tmHeight) + CellPadding * 2;
            const int atlasW = cellW * AtlasColumns;
            const int atlasHPerWeight = cellH * AtlasRows;
            const int atlasH = atlasHPerWeight * 2; // Normal's block, then Bold's block.

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = atlasW;
            bmi.bmiHeader.biHeight = -atlasH; // Top-down DIB.
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            void* bits = nullptr;
            HBITMAP dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ oldBitmap = SelectObject(memDC, dib);

            // Clear to black (we only use the atlas as an alpha/coverage mask).
            memset(bits, 0, (size_t)atlasW * atlasH * 4);

            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, RGB(255, 255, 255));

            for (int weightIndex = 0; weightIndex < 2; weightIndex++) {
                auto& w = weights[weightIndex];
                const int rowOffset = weightIndex * AtlasRows;

                // Re-select the font for this weight now that it's drawing into the real DIB
                // section (some GDI implementations reset font smoothing state when the target
                // surface changes).
                SelectObject(memDC, w.font);

                for (int i = 0; i < GlyphCount; i++) {
                    const wchar_t ch = FirstGlyph + i;
                    const int col = i % AtlasColumns;
                    const int row = i / AtlasColumns + rowOffset;
                    // Anchor each glyph at its own baseline (tmAscent below the cell's top
                    // padding) rather than the cell's raw top, so ascenders/descenders land
                    // consistently within the padding on both sides instead of only being safe
                    // on one side.
                    const int cellX = col * cellW + CellPadding;
                    const int cellTop = row * cellH + CellPadding;

                    SIZE extent{};
                    const wchar_t str[2] = {ch, 0};
                    GetTextExtentPoint32W(memDC, str, 1, &extent);
                    ExtTextOutW(memDC, cellX, cellTop, ETO_CLIPPED, nullptr, str, 1, nullptr);

                    INT advanceWidth = extent.cx;
                    GetCharWidth32W(memDC, ch, ch, &advanceWidth);

                    GlyphInfo& g = w.glyphs[i];
                    g.advance = (float)advanceWidth;
                    g.u0 = (float)(col * cellW) / atlasW;
                    g.v0 = (float)(row * cellH) / atlasH;
                    g.u1 = (float)(col * cellW + cellW) / atlasW;
                    g.v1 = (float)(row * cellH + cellH) / atlasH;
                    // Use the full cell as the quad size so glyphs aren't clipped; advance stays
                    // based on the font's real metrics so spacing looks correct. (drawString()
                    // shrinks the actually-drawn quad back down closer to the advance - see
                    // there for why the cell itself still needs to be this wide.)
                    g.width = (float)cellW;
                    g.height = (float)cellH;
                }
            }

            // Extract the blue channel (any channel works - GDI wrote greyscale into RGB)
            // as our R8 coverage atlas.
            std::vector<uint8_t> atlasR8((size_t)atlasW * atlasH);
            const uint8_t* src = reinterpret_cast<const uint8_t*>(bits);
            for (size_t p = 0; p < atlasR8.size(); p++) {
                atlasR8[p] = src[p * 4 + 0]; // BGRA -> B
            }

            SelectObject(memDC, oldFontForMetrics);
            for (auto& w : weights) {
                DeleteObject(w.font);
            }
            SelectObject(memDC, oldBitmap);
            DeleteObject(dib);
            DeleteDC(memDC);
            ReleaseDC(nullptr, screenDC);

            uploadAtlasTexture(atlasR8.data(), atlasW, atlasH);
        }

        void uploadAtlasTexture(const uint8_t* pixels, int width, int height) {
            const D3D12_HEAP_PROPERTIES defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8_UNORM, width, height, 1, 1);
            CHECK_HRCMD(m_device->CreateCommittedResource(&defaultHeap,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &texDesc,
                                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                                          nullptr,
                                                          IID_PPV_ARGS(set(m_atlasTexture))));
            SetDebugName(get(m_atlasTexture), "Text atlas TEX2D");

            const UINT64 uploadSize = GetRequiredIntermediateSize(get(m_atlasTexture), 0, 1);
            const D3D12_HEAP_PROPERTIES uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const D3D12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
            ComPtr<ID3D12Resource> atlasStaging;
            CHECK_HRCMD(m_device->CreateCommittedResource(&uploadHeap,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &uploadDesc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ,
                                                          nullptr,
                                                          IID_PPV_ARGS(set(atlasStaging))));

            // One-shot upload using its own tiny command list/allocator/fence, since
            // initialize() runs once, off the main per-frame command list.
            ComPtr<ID3D12CommandAllocator> allocator;
            CHECK_HRCMD(
                m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(set(allocator))));
            ComPtr<ID3D12GraphicsCommandList> uploadCommandList;
            CHECK_HRCMD(m_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, get(allocator), nullptr, IID_PPV_ARGS(set(uploadCommandList))));

            D3D12_SUBRESOURCE_DATA subresourceData{};
            subresourceData.pData = pixels;
            subresourceData.RowPitch = width;
            subresourceData.SlicePitch = (size_t)width * height;
            UpdateSubresources(
                get(uploadCommandList), get(m_atlasTexture), get(atlasStaging), 0, 0, 1, &subresourceData);

            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                get(m_atlasTexture), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            uploadCommandList->ResourceBarrier(1, &barrier);
            CHECK_HRCMD(uploadCommandList->Close());

            ID3D12CommandList* lists[] = {get(uploadCommandList)};
            m_uploadQueue->ExecuteCommandLists(1, lists);

            // Block until the upload completes - this only happens once, at startup.
            ComPtr<ID3D12Fence> fence;
            CHECK_HRCMD(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(set(fence))));
            HANDLE fenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
            CHECK_HRCMD(m_uploadQueue->Signal(get(fence), 1));
            CHECK_HRCMD(fence->SetEventOnCompletion(1, fenceEvent));
            WaitForSingleObject(fenceEvent, INFINITE);
            CloseHandle(fenceEvent);
        }

        void createRootSignature() {
            // Root signature: t0 (atlas SRV, table) + 2 root constants (screen-to-clip scale,
            // matching the HLSL cbuffer's float2 screenToClip below - was 4 for no reason, which
            // left 2 unused/uninitialized DWORDs in the root constant block).
            CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            CD3DX12_ROOT_PARAMETER params[2];
            params[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
            params[1].InitAsConstants(2, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);

            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            CD3DX12_ROOT_SIGNATURE_DESC rsDesc(2,
                                               params,
                                               1,
                                               &sampler,
                                               D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

            ComPtr<ID3DBlob> signature, errors;
            HRESULT hr =
                D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, set(signature), set(errors));
            if (FAILED(hr)) {
                if (errors) {
                    toolkit::log::Log("Text renderer root signature error: %s\n", (char*)errors->GetBufferPointer());
                }
                CHECK_HRESULT(hr, "Failed to serialize text renderer root signature");
            }
            CHECK_HRCMD(m_device->CreateRootSignature(
                0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(set(m_rootSignature))));
        }

        // Builds (or rebuilds, if the format or sample count changed) the PSO. Called lazily
        // from beginText() because the render target format isn't known until then.
        //
        // The sample count matters just as much as the format: this class draws onto whatever
        // texture the caller most recently bound via setRenderTargets(), and that isn't always
        // the toolkit's own dedicated (always single-sampled) menu quad swapchain - the legacy
        // menu mode draws directly onto the application's own swapchain image, which can be
        // multisampled if the application (or the user, via its in-game MSAA setting) requested
        // it. A PSO built with SampleDesc.Count=1 against a multisampled render target is a
        // mismatch: ID3D12GraphicsCommandList::DrawInstanced with that PSO bound produces no
        // visible pixels (this was silently swallowed here, so it only showed up as "the menu
        // background/highlight boxes render via ClearRenderTargetView - which does not care
        // about sample count - but no glyph ever appears" with legacy menu mode enabled).
        // In practice this runs once per distinct (format, sampleCount) pair actually used -
        // the cost of the check on every beginText() call is negligible.
        void ensurePipeline(DXGI_FORMAT rtvFormat, uint32_t sampleCount) {
            if (m_pipelineState && m_rtvFormat == rtvFormat && m_sampleCount == sampleCount) {
                return;
            }
            m_rtvFormat = rtvFormat;
            m_sampleCount = sampleCount;

            // clang-format off
            static constexpr char ShaderSource[] = R"(
                cbuffer Constants : register(b0) {
                    float2 screenToClip;
                };
                Texture2D atlasTexture : register(t0);
                SamplerState atlasSampler : register(s0);

                struct VSInput {
                    float2 position : POSITION;
                    float2 uv : TEXCOORD0;
                    float4 color : COLOR0;
                };
                struct PSInput {
                    float4 position : SV_POSITION;
                    float2 uv : TEXCOORD0;
                    float4 color : COLOR0;
                };

                PSInput VSMain(VSInput input) {
                    PSInput output;
                    // Pixel space (top-left origin) -> clip space.
                    output.position = float4(input.position.x * screenToClip.x - 1.0,
                                             1.0 - input.position.y * screenToClip.y,
                                             0.0, 1.0);
                    output.uv = input.uv;
                    output.color = input.color;
                    return output;
                }

                float4 PSMain(PSInput input) : SV_TARGET {
                    float coverage = atlasTexture.Sample(atlasSampler, input.uv).r;
                    return float4(input.color.rgb, input.color.a * coverage);
                }
            )";
            // clang-format on

            ComPtr<ID3DBlob> vsBytes, psBytes, shaderErrors;
            HRESULT hr = D3DCompile(ShaderSource,
                            sizeof(ShaderSource),
                            nullptr,
                            nullptr,
                            nullptr,
                            "VSMain",
                            "vs_5_0",
                            0,
                            0,
                            set(vsBytes),
                            set(shaderErrors));
            if (FAILED(hr)) {
                if (shaderErrors) {
                    toolkit::log::Log("Text renderer VS compile error: %s\n", (char*)shaderErrors->GetBufferPointer());
                }
                CHECK_HRESULT(hr, "Failed to compile text renderer vertex shader");
            }
            hr = D3DCompile(ShaderSource,
                            sizeof(ShaderSource),
                            nullptr,
                            nullptr,
                            nullptr,
                            "PSMain",
                            "ps_5_0",
                            0,
                            0,
                            set(psBytes),
                            set(shaderErrors));
            if (FAILED(hr)) {
                if (shaderErrors) {
                    toolkit::log::Log("Text renderer PS compile error: %s\n", (char*)shaderErrors->GetBufferPointer());
                }
                CHECK_HRESULT(hr, "Failed to compile text renderer pixel shader");
            }

            const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
            psoDesc.pRootSignature = get(m_rootSignature);
            psoDesc.VS = {vsBytes->GetBufferPointer(), vsBytes->GetBufferSize()};
            psoDesc.PS = {psBytes->GetBufferPointer(), psBytes->GetBufferSize()};
            psoDesc.InputLayout = {inputLayout, ARRAYSIZE(inputLayout)};
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            psoDesc.NumRenderTargets = 1;
            psoDesc.RTVFormats[0] = m_rtvFormat;
            psoDesc.SampleDesc.Count = m_sampleCount;
            psoDesc.SampleMask = UINT_MAX;

            psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            psoDesc.DepthStencilState.DepthEnable = FALSE;
            psoDesc.DepthStencilState.StencilEnable = FALSE;

            psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            auto& rt0 = psoDesc.BlendState.RenderTarget[0];
            rt0.BlendEnable = TRUE;
            rt0.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt0.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt0.BlendOp = D3D12_BLEND_OP_ADD;
            rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt0.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

            CHECK_HRCMD(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(set(m_pipelineState))));
            SetDebugName(get(m_pipelineState), "Text renderer PSO");
        }

        void createUploadHeapAndDescriptors() {
            // SRV heap: one shader-visible descriptor for the atlas.
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = 1;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            CHECK_HRCMD(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(set(m_srvHeap))));

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            m_device->CreateShaderResourceView(
                get(m_atlasTexture), &srvDesc, m_srvHeap->GetCPUDescriptorHandleForHeapStart());

            // Dynamic (upload-heap) vertex buffer, sized generously for one frame's worth of
            // glyph quads. 8192 chars * 6 verts * sizeof(TextVertex) covers the vast majority
            // of the toolkit's menu screens (see flushText() for what happens if a screen ever
            // exceeds this).
            //
            // The buffer holds TWO such slots, ping-ponged per flushText() call (see there) so
            // that writing this frame's vertices never overlaps the GPU still reading last
            // flush's draw out of the same memory.
            m_uploadSlotSize = 8192 * 6 * sizeof(TextVertex);
            const D3D12_HEAP_PROPERTIES uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(m_uploadSlotSize * 2);
            CHECK_HRCMD(m_device->CreateCommittedResource(&uploadHeap,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &bufferDesc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ,
                                                          nullptr,
                                                          IID_PPV_ARGS(set(m_vertexUploadBuffer))));
            SetDebugName(get(m_vertexUploadBuffer), "Text renderer vertex upload buffer");
        }

        ID3D12Device* m_device{nullptr};
        ID3D12CommandQueue* m_uploadQueue{nullptr};
        DXGI_FORMAT m_rtvFormat{DXGI_FORMAT_UNKNOWN};
        uint32_t m_sampleCount{1};

        GlyphInfo m_glyphs[GlyphCount]{};
        GlyphInfo m_glyphsBold[GlyphCount]{};
        ComPtr<ID3D12Resource> m_atlasTexture;
        ComPtr<ID3D12DescriptorHeap> m_srvHeap;

        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_pipelineState;

        ComPtr<ID3D12Resource> m_vertexUploadBuffer;
        size_t m_uploadSlotSize{0}; // Capacity of ONE of the two ping-ponged slots (see flushText()).
        uint32_t m_currentUploadSlot{0};
        std::vector<TextVertex> m_pendingVertices;

        // Set per-beginText() call; cleared by flushText().
        ID3D12GraphicsCommandList* m_commandList{nullptr};
        D3D12_CPU_DESCRIPTOR_HANDLE m_renderTargetView{};
        int32_t m_viewportX{0};
        int32_t m_viewportY{0};
        uint32_t m_viewportWidth{0};
        uint32_t m_viewportHeight{0};
    };

} // namespace toolkit::graphics::d3d12text
