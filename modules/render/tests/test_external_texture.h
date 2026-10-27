#pragma once

#include "test_external_resource.h"
#include "external_texture_test_utils.h"

namespace radray::render {

struct ExternalTextureParameter {
    test::ExternalTextureCase Case;
    int Route;
    bool LocalControl{false};
};
void PrintTo(const ExternalTextureParameter& value, std::ostream* output);

class ExternalResourceTextureTest : public ExternalResourceFixture, public testing::WithParamInterface<ExternalTextureParameter> {
protected:
    int Route() const override;
    void SetUp() override;
    test::ExternalTextureCase Case;
};

}  // namespace radray::render
