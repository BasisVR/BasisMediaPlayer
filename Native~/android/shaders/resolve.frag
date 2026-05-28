#version 450
/* Sample the imported AHardwareBuffer through an immutable Y'CbCr-conversion
 * sampler (binding 0). The conversion does YCbCr->RGB on sample, so texture()
 * already returns linear-ish RGB; we just force opaque alpha. */
layout(binding = 0) uniform sampler2D srcYcbcr;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(texture(srcYcbcr, vUV).rgb, 1.0);
}
