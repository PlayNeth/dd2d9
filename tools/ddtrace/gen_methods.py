# Generates methods.inc (next to this script): each interface's methods in table order,
# with how many arguments each takes after `this` (every argument is 4 bytes on x86).
# Run again after changing the lists; methods.inc is committed so the build needs no Python.
import os
unk = [('QueryInterface',2),('AddRef',0),('Release',0)]
dd1 = [('Compact',0),('CreateClipper',3),('CreatePalette',4),('CreateSurface',3),('DuplicateSurface',2),
       ('EnumDisplayModes',4),('EnumSurfaces',4),('FlipToGDISurface',0),('GetCaps',2),('GetDisplayMode',1),
       ('GetFourCCCodes',2),('GetGDISurface',1),('GetMonitorFrequency',1),('GetScanLine',1),
       ('GetVerticalBlankStatus',1),('Initialize',1),('RestoreDisplayMode',0),('SetCooperativeLevel',2),
       ('SetDisplayMode',3),('WaitForVerticalBlank',2)]
dd2 = dd1[:-2] + [('SetDisplayMode',5),('WaitForVerticalBlank',2),('GetAvailableVidMem',3)]
dd4 = dd2 + [('GetSurfaceFromDC',2),('RestoreAllSurfaces',0),('TestCooperativeLevel',0),('GetDeviceIdentifier',2)]
dd7 = dd4 + [('StartModeTest',3),('EvaluateMode',2)]
s1 = [('AddAttachedSurface',1),('AddOverlayDirtyRect',1),('Blt',5),('BltBatch',3),('BltFast',5),
      ('DeleteAttachedSurface',2),('EnumAttachedSurfaces',2),('EnumOverlayZOrders',3),('Flip',2),
      ('GetAttachedSurface',2),('GetBltStatus',1),('GetCaps',1),('GetClipper',1),('GetColorKey',2),('GetDC',1),
      ('GetFlipStatus',1),('GetOverlayPosition',2),('GetPalette',1),('GetPixelFormat',1),('GetSurfaceDesc',1),
      ('Initialize',2),('IsLost',0),('Lock',4),('ReleaseDC',1),('Restore',0),('SetClipper',1),('SetColorKey',2),
      ('SetOverlayPosition',2),('SetPalette',1),('Unlock',1),('UpdateOverlay',5),('UpdateOverlayDisplay',1),
      ('UpdateOverlayZOrder',2)]
s2 = s1 + [('GetDDInterface',1),('PageLock',1),('PageUnlock',1)]
s3 = s2 + [('SetSurfaceDesc',2)]
s4 = s3 + [('SetPrivateData',4),('GetPrivateData',3),('FreePrivateData',1),('GetUniquenessValue',1),('ChangeUniquenessValue',0)]
s7 = s4 + [('SetPriority',1),('GetPriority',1),('SetLOD',1),('GetLOD',1)]
d3d7 = [('EnumDevices',2),('CreateDevice',3),('CreateVertexBuffer',3),('EnumZBufferFormats',3),('EvictManagedTextures',0)]
dev7 = [('GetCaps',1),('EnumTextureFormats',2),('BeginScene',0),('EndScene',0),('GetDirect3D',1),('SetRenderTarget',2),
        ('GetRenderTarget',1),('Clear',6),('SetTransform',2),('GetTransform',2),('SetViewport',1),('MultiplyTransform',2),
        ('GetViewport',1),('SetMaterial',1),('GetMaterial',1),('SetLight',2),('GetLight',2),('SetRenderState',2),
        ('GetRenderState',2),('BeginStateBlock',0),('EndStateBlock',1),('PreLoad',1),('DrawPrimitive',5),
        ('DrawIndexedPrimitive',7),('SetClipStatus',1),('GetClipStatus',1),('DrawPrimitiveStrided',5),
        ('DrawIndexedPrimitiveStrided',7),('DrawPrimitiveVB',5),('DrawIndexedPrimitiveVB',7),
        ('ComputeSphereVisibility',5),('GetTexture',2),('SetTexture',2),('GetTextureStageState',3),
        ('SetTextureStageState',3),('ValidateDevice',1),('ApplyStateBlock',1),('CaptureStateBlock',1),
        ('DeleteStateBlock',1),('CreateStateBlock',2),('Load',5),('LightEnable',2),('GetLightEnable',2),
        ('SetClipPlane',2),('GetClipPlane',2),('GetInfo',3)]
vb7 = [('Lock',3),('Unlock',0),('ProcessVertices',7),('GetVertexBufferDesc',1),('Optimize',2),('ProcessVerticesStrided',7)]
pal = [('GetCaps',1),('GetEntries',4),('Initialize',3),('SetEntries',4)]
clip = [('GetClipList',3),('GetHWnd',1),('Initialize',2),('IsClipListChanged',1),('SetClipList',2),('SetHWnd',2)]
gamma = [('GetGammaRamp',2),('SetGammaRamp',2)]

d3d1 = [('Initialize',1),('EnumDevices',2),('CreateLight',2),('CreateMaterial',2),('CreateViewport',2),('FindDevice',2)]
d3d2 = [('EnumDevices',2),('CreateLight',2),('CreateMaterial',2),('CreateViewport',2),('FindDevice',2),('CreateDevice',3)]
d3d3 = [('EnumDevices',2),('CreateLight',2),('CreateMaterial',2),('CreateViewport',2),('FindDevice',2),('CreateDevice',4),
        ('CreateVertexBuffer',4),('EnumZBufferFormats',3),('EvictManagedTextures',0)]
dev1 = [('Initialize',3),('GetCaps',2),('SwapTextureHandles',2),('CreateExecuteBuffer',3),('GetStats',1),('Execute',3),
        ('AddViewport',1),('DeleteViewport',1),('NextViewport',3),('Pick',4),('GetPickRecords',2),('EnumTextureFormats',2),
        ('CreateMatrix',1),('SetMatrix',2),('GetMatrix',2),('DeleteMatrix',1),('BeginScene',0),('EndScene',0),('GetDirect3D',1)]
dev2 = [('GetCaps',2),('SwapTextureHandles',2),('GetStats',1),('AddViewport',1),('DeleteViewport',1),('NextViewport',3),
        ('EnumTextureFormats',2),('BeginScene',0),('EndScene',0),('GetDirect3D',1),('SetCurrentViewport',1),
        ('GetCurrentViewport',1),('SetRenderTarget',2),('GetRenderTarget',1),('Begin',3),('BeginIndexed',5),('Vertex',1),
        ('Index',1),('End',1),('GetRenderState',2),('SetRenderState',2),('GetLightState',2),('SetLightState',2),
        ('SetTransform',2),('GetTransform',2),('MultiplyTransform',2),('DrawPrimitive',5),('DrawIndexedPrimitive',7),
        ('SetClipStatus',1),('GetClipStatus',1)]
dev3 = [d for d in dev2 if d[0] != 'SwapTextureHandles'] + [('DrawPrimitiveStrided',5),('DrawIndexedPrimitiveStrided',7),
        ('DrawPrimitiveVB',5),('DrawIndexedPrimitiveVB',5),('ComputeSphereVisibility',5),('GetTexture',2),('SetTexture',2),
        ('GetTextureStageState',3),('SetTextureStageState',3),('ValidateDevice',1)]
vb1 = [('Lock',3),('Unlock',0),('ProcessVertices',7),('GetVertexBufferDesc',1),('Optimize',2)]
vp1 = [('Initialize',1),('GetViewport',1),('SetViewport',1),('TransformVertices',4),('LightElements',2),('SetBackground',1),
       ('GetBackground',2),('SetBackgroundDepth',1),('GetBackgroundDepth',2),('Clear',3),('AddLight',1),('DeleteLight',1),
       ('NextLight',3)]
vp2 = vp1 + [('GetViewport2',1),('SetViewport2',1)]
vp3 = vp2 + [('SetBackgroundDepth2',1),('GetBackgroundDepth2',2),('Clear2',6)]
tex1 = [('Initialize',2),('GetHandle',2),('PaletteChanged',2),('Load',1),('Unload',0)]
tex2 = [('GetHandle',2),('PaletteChanged',2),('Load',1)]
mat1 = [('Initialize',1),('SetMaterial',1),('GetMaterial',1),('GetHandle',2),('Reserve',0),('Unreserve',0)]
mat2 = [('SetMaterial',1),('GetMaterial',1),('GetHandle',2)]
light = [('Initialize',1),('SetLight',1),('GetLight',1)]
xbuf = [('Initialize',2),('Lock',1),('Unlock',0),('SetExecuteData',1),('GetExecuteData',1),('Validate',4),('Optimize',1)]
ifaces = [('DD1',dd1),('DD2',dd2),('DD4',dd4),('DD7',dd7),('Surf1',s1),('Surf2',s2),('Surf3',s3),('Surf4',s4),
          ('Surf7',s7),('D3D7',d3d7),('Dev7',dev7),('VB7',vb7),('Pal',pal),('Clip',clip),('Gamma',gamma),('D3D1',d3d1),('D3D2',d3d2),('D3D3',d3d3),('Dev1',dev1),('Dev2',dev2),('Dev3',dev3),('VB1',vb1),('VP1',vp1),('VP2',vp2),('VP3',vp3),('Tex1',tex1),('Tex2',tex2),('Mat1',mat1),('Mat2',mat2),('Mat3',mat2),('Light',light),('XBuf',xbuf)]
out = ['// Generated by gen_methods.py: edit that and run it again.']
out.append('enum Iface { %s, kIfaces };' % ', '.join('k'+n for n,_ in ifaces))
out.append('static const int kMax = %d;' % max(len(unk)+len(m) for _,m in ifaces))
for n,m in ifaces:
    full = unk+m
    out.append('static const Method k%sMethods[] = {%s};' % (n, ', '.join('{"%s", %d}'%(a,c) for a,c in full)))
out.append('static const IfaceInfo kInfo[kIfaces] = {%s};' % ', '.join('{"%s", k%sMethods, %d}'%(n,n,len(unk)+len(m)) for n,m in ifaces))
hooks=[]
for n,m in ifaces:
    full=unk+m
    hooks.append('{%s}' % ', '.join('reinterpret_cast<void*>(&H<k%s, %d, std::make_index_sequence<%d>>::Fn)'%(n,i,c) for i,(a,c) in enumerate(full)))
out.append('static void* const kHooks[kIfaces][kMax] = {%s};' % ',\n'.join(hooks))
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'methods.inc'), 'w').write('\n'.join(out)+'\n')
