//
//  OpenEphysPlugin.cpp
//  OpenEphys
//
//  Created by Christopher Stawarz on 7/22/15.
//  Copyright (c) 2015 The MWorks Project. All rights reserved.
//


BEGIN_NAMESPACE_MW


class OpenEphysPlugin : public Plugin {
    void registerComponents(boost::shared_ptr<ComponentRegistry> registry) override {
    }
};


extern "C" Plugin* getPlugin() {
    return new OpenEphysPlugin();
}


END_NAMESPACE_MW
