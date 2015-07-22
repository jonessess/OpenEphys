//
//  OpenEphysInterface.h
//  OpenEphys
//
//  Created by Christopher Stawarz on 7/22/15.
//  Copyright (c) 2015 The MWorks Project. All rights reserved.
//

#ifndef __OpenEphys__OpenEphysInterface__
#define __OpenEphys__OpenEphysInterface__


BEGIN_NAMESPACE_MW


class OpenEphysInterface : public IODevice, boost::noncopyable {
    
public:
    static const std::string HOSTNAME;
    static const std::string PORT;
    static const std::string SYNC;
    static const std::string SYNC_CHANNELS;
    static const std::string SPIKES;
    
    static void describeComponent(ComponentInfo &info);
    
    explicit OpenEphysInterface(const ParameterValueMap &parameters);
    ~OpenEphysInterface();
    
    bool initialize() override;
    bool startDeviceIO() override;
    bool stopDeviceIO() override;
    
private:
    static constexpr std::uint8_t TTL = 3;
    static constexpr std::uint8_t SPIKE = 4;
    
    static constexpr MWTime syncInterval = 1000000;  // 1 second
    
    bool subscribeToEventType(std::uint8_t type);
    void handleEvents();
    void terminateEventHandlerThread();
    
    const std::string endpoint;
    const VariablePtr sync;
    std::vector<std::uint8_t> syncChannels;
    VariablePtr spikes;
    
    std::unique_ptr<void, decltype(&zmq_ctx_term)> zmqContext;
    std::unique_ptr<void, decltype(&zmq_close)> zmqSocket;
    boost::thread eventHandlerThread;
    
    using scoped_lock = boost::mutex::scoped_lock;
    boost::mutex mutex;
    
    bool running;
    
};


END_NAMESPACE_MW


#endif /* !defined(__OpenEphys__OpenEphysInterface__) */



























