#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <radray/runtime/components/spot_light_component.h>

namespace radray {
namespace {

TEST(SpotLight, ConeAndRadiusRejectInvalidParameters) {
    SpotLightComponent component;
    ASSERT_TRUE(component.SetConeAngles(.2f, .6f));
    component.SetAttenuationRadius(3);
    component.SetCastShadow(false);
    EXPECT_FALSE(component.SetConeAngles(.7f, .3f));
    EXPECT_FALSE(component.SetConeAngles(0, 2));
    EXPECT_FALSE(component.SetConeAngles(0, std::numeric_limits<float>::quiet_NaN()));
    component.SetAttenuationRadius(std::numeric_limits<float>::infinity());
    EXPECT_FLOAT_EQ(component.GetAttenuationRadius(), 3);

}

TEST(SpotLight, InvalidRadiusPreservesPreviousValue) {
    SpotLightComponent component;
    component.SetAttenuationRadius(3);
    for (const float radius : {-1.f, std::numeric_limits<float>::quiet_NaN()}) {
        component.SetAttenuationRadius(radius);
        EXPECT_FLOAT_EQ(component.GetAttenuationRadius(), 3);
    }
    component.SetAttenuationRadius(0);
    EXPECT_FLOAT_EQ(component.GetAttenuationRadius(), 0);

}

}  // namespace
}  // namespace radray
