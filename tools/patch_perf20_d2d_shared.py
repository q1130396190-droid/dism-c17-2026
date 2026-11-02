import pathlib

p = pathlib.Path('DuiLib/Core/UIRender.cpp')
raw = p.read_bytes()
pathlib.Path('DuiLib/Core/UIRender.cpp.bak_perf20').write_bytes(raw)
text = raw.decode('utf-8', errors='surrogateescape')
NL = '\r\n'

# ---------------------------------------------------------------- 1) shared device state
old_state_head = (
    '\t\tstruct D2DTextState' + NL +
    '\t\t{' + NL +
    '\t\t\tID2D1Factory*        pFactory = nullptr;' + NL +
    '\t\t\tIDWriteFactory*      pWrite = nullptr;' + NL +
    '\t\t\tID2D1DCRenderTarget* pTarget = nullptr;' + NL +
    '\t\t\tbool                 bTried = false;' + NL +
    '\t\t\tbool                 bOk = false;' + NL +
    '\t\t\tbool                 bEnabled = false;' + NL)
assert text.count(old_state_head) == 1

new_state_head = (
    '\t\t// [PERF-20] Device-side objects are process-wide, not per thread.' + NL +
    '\t\t// CreateDCRenderTarget alone costs 94-110ms on this machine: it brings' + NL +
    '\t\t// up the WARP/DXGI device. Measured invariant across single- and' + NL +
    '\t\t// multi-threaded factories, so moving threads cannot make it cheaper.' + NL +
    '\t\t// The only useful fix is to pay it OFF the startup critical path, which' + NL +
    '\t\t// requires one shared device that a warm-up thread can build while the' + NL +
    '\t\t// demo is still creating its window.' + NL +
    '\t\tstruct D2DDeviceShared' + NL +
    '\t\t{' + NL +
    '\t\t\tID2D1Factory*        pFactory = nullptr;' + NL +
    '\t\t\tIDWriteFactory*      pWrite = nullptr;' + NL +
    '\t\t\tID2D1DCRenderTarget* pTarget = nullptr;' + NL +
    '\t\t\tPFN_ALPHABLEND       pfnAlphaBlend = nullptr;' + NL +
    '\t\t\tvolatile LONG        nState = 0;   // 0 idle, 1 building, 2 ready, 3 failed' + NL +
    '\t\t\tvolatile LONG        bEnabled = 0;' + NL +
    '\t\t};' + NL +
    NL +
    '\t\tD2DDeviceShared& D2DShared()' + NL +
    '\t\t{' + NL +
    '\t\t\t// Function-local static: thread-safe init, destroyed at process exit.' + NL +
    '\t\t\tstatic D2DDeviceShared s;' + NL +
    '\t\t\treturn s;' + NL +
    '\t\t}' + NL +
    NL +
    '\t\tstruct D2DTextState' + NL +
    '\t\t{' + NL +
    '\t\t\tbool                 bOk = false;' + NL)
text = text.replace(old_state_head, new_state_head)

# ---------------------------------------------------------------- 2) per-thread fields cleanup
# The per-thread struct keeps formats/cache/scratch; drop the AlphaBlend member
# that moved to the shared device and point readers at D2DShared().
old_alpha = ('\t\t\tPFN_ALPHABLEND pfnAlphaBlend = nullptr;     // [WIN11-03b]' + NL)
assert text.count(old_alpha) == 1
text = text.replace(old_alpha, '')

# ---------------------------------------------------------------- 3) rewrite EnsureD2DText
old_ensure_start = '\t\tvoid EnsureD2DText()' + NL + '\t\t{' + NL
idx = text.index(old_ensure_start)
end_marker = '\t\t\tst.bOk = true;' + NL + '\t\t}' + NL
end = text.index(end_marker, idx)
end += len(end_marker)

new_ensure = (
    '\t\t// [PERF-20] Build the process-wide D2D device exactly once.' + NL +
    '\t\tvoid BuildD2DDeviceOnce()' + NL +
    '\t\t{' + NL +
    '\t\t\tD2DDeviceShared& sh = D2DShared();' + NL +
    '\t\t\tif (::InterlockedCompareExchange(&sh.nState, 1, 0) != 0) return;' + NL +
    NL +
    '\t\t\tHMODULE hD2D = ::LoadLibraryW(L"d2d1.dll");' + NL +
    '\t\t\tHMODULE hDW = ::LoadLibraryW(L"dwrite.dll");' + NL +
    '\t\t\tif (hD2D == nullptr || hDW == nullptr) { ::InterlockedExchange(&sh.nState, 3); return; }' + NL +
    NL +
    '\t\t\ttypedef HRESULT(WINAPI* PFN_D2D1CreateFactory)(D2D1_FACTORY_TYPE, REFIID, const void*, void**);' + NL +
    '\t\t\ttypedef HRESULT(WINAPI* PFN_DWriteCreateFactory)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);' + NL +
    '\t\t\tauto pfnD2D = reinterpret_cast<PFN_D2D1CreateFactory>(::GetProcAddress(hD2D, "D2D1CreateFactory"));' + NL +
    '\t\t\tauto pfnDW = reinterpret_cast<PFN_DWriteCreateFactory>(::GetProcAddress(hDW, "DWriteCreateFactory"));' + NL +
    '\t\t\tif (pfnD2D == nullptr || pfnDW == nullptr) { ::InterlockedExchange(&sh.nState, 3); return; }' + NL +
    NL +
    '\t\t\t// Multi-threaded factory: the object is created by the warm-up thread' + NL +
    '\t\t\t// and used by the UI thread, which a single-threaded factory forbids.' + NL +
    '\t\t\tif (FAILED(pfnD2D(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory),' + NL +
    '\t\t\t                  nullptr, reinterpret_cast<void**>(&sh.pFactory))) || sh.pFactory == nullptr)' + NL +
    '\t\t\t{ ::InterlockedExchange(&sh.nState, 3); return; }' + NL +
    '\t\t\tif (FAILED(pfnDW(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),' + NL +
    '\t\t\t                  reinterpret_cast<IUnknown**>(&sh.pWrite))) || sh.pWrite == nullptr)' + NL +
    '\t\t\t{ ::InterlockedExchange(&sh.nState, 3); return; }' + NL +
    NL +
    '\t\t\tD2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(' + NL +
    '\t\t\t\tD2D1_RENDER_TARGET_TYPE_DEFAULT,' + NL +
    '\t\t\t\tD2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));' + NL +
    '\t\t\tif (FAILED(sh.pFactory->CreateDCRenderTarget(&props, &sh.pTarget)) || sh.pTarget == nullptr)' + NL +
    '\t\t\t{ ::InterlockedExchange(&sh.nState, 3); return; }' + NL +
    NL +
    '\t\t\t// [WIN11-03b] resolve AlphaBlend once; the cached path and the fresh' + NL +
    '\t\t\t// path both need it. Leaving this null silently disables cache hits.' + NL +
    '\t\t\tHMODULE hMs = ::GetModuleHandleW(L"msimg32.dll");' + NL +
    '\t\t\tif (hMs == nullptr) hMs = ::LoadLibraryW(L"msimg32.dll");' + NL +
    '\t\t\tif (hMs != nullptr)' + NL +
    '\t\t\t\tsh.pfnAlphaBlend = reinterpret_cast<PFN_ALPHABLEND>(' + NL +
    '\t\t\t\t\t::GetProcAddress(hMs, "AlphaBlend"));' + NL +
    NL +
    '\t\t\t::InterlockedExchange(&sh.nState, 2);' + NL +
    '\t\t}' + NL +
    NL +
    '\t\t// The UI thread must still have a usable device even if warm-up never ran.' + NL +
    '\t\tvoid EnsureD2DText()' + NL +
    '\t\t{' + NL +
    '\t\t\tD2DDeviceShared& sh = D2DShared();' + NL +
    '\t\t\tD2DTextState& st = D2DState();' + NL +
    '\t\t\tif (st.bOk) return;' + NL +
    '\t\t\tBuildD2DDeviceOnce();' + NL +
    '\t\t\tst.bOk = (::InterlockedCompareExchange(&sh.nState, 2, 2) == 2);' + NL +
    '\t\t}' + NL)
text = text[:idx] + new_ensure + text[end:]

p.write_bytes(text.encode('utf-8', errors='surrogateescape'))
print('stage 1 done')
