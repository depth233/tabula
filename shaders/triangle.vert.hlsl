struct VSOut {
  float4 pos : SV_Position;
  float3 col : COLOR0;
};

struct Push {
  float2 extentPx; // swapchain extent in pixels
  int hover;       // 0 none, 1 min, 2 max, 3 close
  float _pad;
};

[[vk::push_constant]] Push pc;

static float2 pxToNdc(float2 p) {
  // p: pixel coordinates in [0..extent]
  float2 ndc = (p / pc.extentPx) * 2.0 - 1.0;
  return ndc;
}

static void quad(uint local, float2 tl, float2 br, out float2 pos) {
  // local: 0..5 (two triangles)
  float2 a = tl;
  float2 b = float2(br.x, tl.y);
  float2 c = br;
  float2 d = float2(tl.x, br.y);
  // 0:a 1:b 2:c 3:a 4:c 5:d
  if (local == 0) pos = a;
  else if (local == 1) pos = b;
  else if (local == 2) pos = c;
  else if (local == 3) pos = a;
  else if (local == 4) pos = c;
  else pos = d;
}

static void rotRect(uint local, float2 center, float halfLen, float halfThick, float angleRad, out float2 pos) {
  // Produces two triangles for a rotated rectangle.
  float c = cos(angleRad);
  float s = sin(angleRad);
  float2 ax = float2(c, s) * halfLen;
  float2 ay = float2(-s, c) * halfThick;
  float2 a = center - ax - ay;
  float2 b = center + ax - ay;
  float2 cpt = center + ax + ay;
  float2 d = center - ax + ay;
  // 0:a 1:b 2:c 3:a 4:c 5:d
  if (local == 0) pos = a;
  else if (local == 1) pos = b;
  else if (local == 2) pos = cpt;
  else if (local == 3) pos = a;
  else if (local == 4) pos = cpt;
  else pos = d;
}

VSOut main(uint vid : SV_VertexID) {
  VSOut o;

  // Draw calls:
  // 0..2: triangle
  // 100..105: title bar quad
  // 110..115: min button quad
  // 120..125: max button quad
  // 130..135: close button quad
  // 140..145: min icon quad
  // 150..173: max icon (4 quads)
  // 180..191: close icon (2 diagonals)

  if (vid < 3) {
    float2 positions[3] = {
      float2( 0.0, -0.5),
      float2( 0.5,  0.5),
      float2(-0.5,  0.5)
    };
    float3 colors[3] = {
      float3(1.0, 0.0, 0.0),
      float3(0.0, 1.0, 0.0),
      float3(0.0, 0.0, 1.0)
    };
    o.pos = float4(positions[vid], 0.0, 1.0);
    o.col = colors[vid];
    return o;
  }

  const float titleH = 18.0;
  const float btn = 14.0;
  const float pad = 2.0;

  float2 tl, br, p;
  float3 baseCol = float3(0.20, 0.22, 0.26);

  if (vid >= 100 && vid < 106) {
    tl = float2(0.0, 0.0);
    br = float2(pc.extentPx.x, titleH);
    quad(vid - 100, tl, br, p);
    o.pos = float4(pxToNdc(p), 0.0, 1.0);
    o.col = float3(0.14, 0.15, 0.18);
    return o;
  }

  // Buttons anchored to top-right.
  float xClose = pc.extentPx.x - pad - btn;
  float xMax = xClose - pad - btn;
  float xMin = xMax - pad - btn;
  float y0 = (titleH - btn) * 0.5;

  if (vid >= 110 && vid < 116) {
    tl = float2(xMin, y0); br = float2(xMin + btn, y0 + btn);
    quad(vid - 110, tl, br, p);
    o.pos = float4(pxToNdc(p), 0.0, 1.0);
    o.col = (pc.hover == 1) ? float3(0.35, 0.38, 0.45) : baseCol;
    return o;
  }
  if (vid >= 120 && vid < 126) {
    tl = float2(xMax, y0); br = float2(xMax + btn, y0 + btn);
    quad(vid - 120, tl, br, p);
    o.pos = float4(pxToNdc(p), 0.0, 1.0);
    o.col = (pc.hover == 2) ? float3(0.35, 0.38, 0.45) : baseCol;
    return o;
  }
  if (vid >= 130 && vid < 136) {
    tl = float2(xClose, y0); br = float2(xClose + btn, y0 + btn);
    quad(vid - 130, tl, br, p);
    o.pos = float4(pxToNdc(p), 0.0, 1.0);
    o.col = (pc.hover == 3) ? float3(0.75, 0.25, 0.25) : baseCol;
    return o;
  }

  // --- Icons ---
  if (vid >= 140 && vid < 146) {
    // Minimize: a small horizontal bar near bottom of button
    float2 itl = float2(xMin + 3.0, y0 + btn - 4.0);
    float2 ibr = float2(xMin + btn - 3.0, y0 + btn - 2.5);
    quad(vid - 140, itl, ibr, p);
    o.pos = float4(pxToNdc(p), 0.0, 1.0);
    o.col = float3(0.95, 0.95, 0.97);
    return o;
  }

  if (vid >= 150 && vid < 174) {
    // Maximize: outline rectangle made of 4 thin quads
    uint local = vid - 150; // 0..23
    uint q = local / 6;     // which segment
    uint v = local % 6;     // vertex in quad
    float2 tl = float2(xMax + 3.0, y0 + 3.0);
    float2 br = float2(xMax + btn - 3.0, y0 + btn - 3.0);
    float t = 1.0;
    float2 qtl, qbr;
    if (q == 0) { qtl = tl; qbr = float2(br.x, tl.y + t); }                 // top
    else if (q == 1) { qtl = float2(tl.x, br.y - t); qbr = br; }           // bottom
    else if (q == 2) { qtl = tl; qbr = float2(tl.x + t, br.y); }           // left
    else { qtl = float2(br.x - t, tl.y); qbr = br; }                       // right
    quad(v, qtl, qbr, p);
    o.pos = float4(pxToNdc(p), 0.0, 1.0);
    o.col = float3(0.95, 0.95, 0.97);
    return o;
  }

  if (vid >= 180 && vid < 192) {
    // Close: two diagonals (two rotated rectangles)
    uint local = vid - 180; // 0..11
    uint which = local / 6; // 0 or 1
    uint v = local % 6;
    float2 center = float2(xClose + btn * 0.5, y0 + btn * 0.5);
    float halfLen = 5.5;
    float halfThick = 1.0;
    float angle = (which == 0) ? 0.785398163f : -0.785398163f; // +/- 45deg
    rotRect(v, center, halfLen, halfThick, angle, p);
    o.pos = float4(pxToNdc(p), 0.0, 1.0);
    o.col = float3(0.98, 0.98, 0.99);
    return o;
  }

  // Fallback (shouldn't happen)
  o.pos = float4(0, 0, 0, 1);
  o.col = float3(1, 0, 1);
  return o;
}

