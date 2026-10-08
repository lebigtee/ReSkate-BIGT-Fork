#pragma once
#include <array>
#include <cstdint>

// The game's character light rig (Lighting/Prefabs/Character/PF_LightRig_Ambient), one of whose
// spot lights is the camera light (Extension/CameraLight).
namespace dingosdk::game::build::v20260929::camera_light {
// A spot light entity of the rig: its data at +0x38, its spatial interface at +0x40. The
// interface's slot +0x18 is the spatial entities' transform getter (it copies the matrix at +0x40
// of the interface), its slot +0x10 the light's transform setter (it tells the light's component,
// then keeps the matrix), whose code starts so.
inline constexpr std::uintptr_t light_data = 0x38, light_spatial_interface = 0x40;
inline constexpr std::uintptr_t spatial_transform_getter = 0x2b3fae0;
inline constexpr std::array<unsigned char, 21> light_transform_setter_prefix{
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x48,0x8b,0xda,
    0x48,0x83,0xc1,0x30,0xe8};
// PbrSpotLightEntityData, as its asset lays it out (the loaded data keeps the layout): the cone's
// inner and outer angles (degrees), the range (metres) and the intensity.
inline constexpr std::uintptr_t spot_inner_angle = 0xb4, spot_attenuation_radius = 0xd8, spot_outer_angle = 0xf0,
                                 spot_intensity = 0x110;
} // namespace dingosdk::game::build::v20260929::camera_light
