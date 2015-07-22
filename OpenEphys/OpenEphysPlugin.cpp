//
//  OpenEphysPlugin.cpp
//  OpenEphys
//
//  Created by Christopher Stawarz on 7/22/15.
//  Copyright (c) 2015 The MWorks Project. All rights reserved.
//

#include "OpenEphysInterface.h"


BEGIN_NAMESPACE_MW


class OpenEphysPlugin : public Plugin {
    void registerComponents(boost::shared_ptr<ComponentRegistry> registry) override {
        registry->registerFactory<StandardComponentFactory, OpenEphysInterface>();
    }
};


extern "C" Plugin* getPlugin() {
    return new OpenEphysPlugin();
}


END_NAMESPACE_MW
