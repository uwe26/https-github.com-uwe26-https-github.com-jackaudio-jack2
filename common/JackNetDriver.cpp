/*
Copyright (C) 2008-2011 Romain Moret at Grame

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "JackCompilerDeps.h"
#include "driver_interface.h"
#include "JackNetDriver.h"
#include "JackEngineControl.h"
#include "JackLockedEngine.h"
#include "JackWaitThreadedDriver.h"

using namespace std;

namespace Jack
{
    JackNetDriver::JackNetDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table,
                                const char* ip, int udp_port, const char* mcif, int mtu, int midi_input_ports, int midi_output_ports,
                                char* net_name, uint transport_sync, int network_latency, 
                                int celt_encoding, int opus_encoding, bool auto_save)
            : JackWaiterDriver(name, alias, engine, table), JackNetSlaveInterface(ip, udp_port)
    {
        jack_log("JackNetDriver::JackNetDriver ip %s, port %d", ip, udp_port);

        // Use the hostname if no name parameter was given
        if (strcmp(net_name, "") == 0) {
            GetHostName(net_name, JACK_CLIENT_NAME_SIZE);
        }
        if(strcmp(mcif,DEFAULT_MULTICAST_IF))
            strcpy(fMulticastIF,mcif);

        fParams.fMtu = mtu;
        
        fWantedMIDICaptureChannels = midi_input_ports;
        fWantedMIDIPlaybackChannels = midi_output_ports;
        
        if (celt_encoding > 0) {
            fParams.fSampleEncoder = JackCeltEncoder;
            fParams.fKBps = celt_encoding;
        } else if (opus_encoding > 0) {
            fParams.fSampleEncoder = JackOpusEncoder;
            fParams.fKBps = opus_encoding;
        } else {
            fParams.fSampleEncoder = JackFloatEncoder;
            //fParams.fSampleEncoder = JackIntEncoder;
        }
        strcpy(fParams.fName, net_name);
        fSocket.GetName(fParams.fSlaveNetName);
        fParams.fTransportSync = transport_sync;
        fParams.fNetworkLatency = network_latency;
        fSendTransportData.fState = -1;
        fReturnTransportData.fState = -1;
        fLastTransportState = -1;
        fLastTimebaseMaster = -1;
        fMidiCapturePortList = NULL;
        fMidiPlaybackPortList = NULL;
        fWantedAudioCaptureChannels = -1;
        fWantedAudioPlaybackChannels = -1;
        fAutoSave = auto_save;
#ifdef JACK_MONITOR
        fNetTimeMon = NULL;
        fRcvSyncUst = 0;
#endif
    }

    JackNetDriver::~JackNetDriver()
    {
        delete[] fMidiCapturePortList;
        delete[] fMidiPlaybackPortList;
#ifdef JACK_MONITOR
        delete fNetTimeMon;
#endif
    }

//open, close, attach and detach------------------------------------------------------

    int JackNetDriver::Open(jack_nframes_t buffer_size,
                         jack_nframes_t samplerate,
                         bool capturing,
                         bool playing,
                         int inchannels,
                         int outchannels,
                         bool monitor,
                         const char* capture_driver_name,
                         const char* playback_driver_name,
                         jack_nframes_t capture_latency,
                         jack_nframes_t playback_latency)
    {
        // Keep initial wanted values
        fWantedAudioCaptureChannels = inchannels;
        fWantedAudioPlaybackChannels = outchannels;
        return JackWaiterDriver::Open(buffer_size, samplerate, 
                                    capturing, playing, 
                                    inchannels, outchannels, 
                                    monitor, 
                                    capture_driver_name, playback_driver_name, 
                                    capture_latency, playback_latency);
    }
                         
    int JackNetDriver::Close()
    {
#ifdef JACK_MONITOR
        if (fNetTimeMon) {
            fNetTimeMon->Save();
        }
#endif
        FreeAll();
        return JackWaiterDriver::Close();
    }

    // Attach and Detach are defined as empty methods: port allocation is done when driver actually start (that is in Init)
    int JackNetDriver::Attach()
    {
        return 0;
    }

    int JackNetDriver::Detach()
    {
        return 0;
    }

//init and restart--------------------------------------------------------------------
    /*
        JackNetDriver is wrapped in a JackWaitThreadedDriver decorator that behaves
        as a "dummy driver, until Init method returns.
    */

    bool JackNetDriver::Initialize()
    {
        jack_log("JackNetDriver::Initialize");
        if (fAutoSave) {
            SaveConnections(0);
        }
        FreePorts();

        // New loading, but existing socket, restart the driver
        if (fSocket.IsSocket()) {
            jack_info("Restarting driver...");
            FreeAll();
        }

        // Set the parameters to send
        fParams.fSendAudioChannels = fWantedAudioCaptureChannels;
        fParams.fReturnAudioChannels = fWantedAudioPlaybackChannels;
        
        fParams.fSendMidiChannels = fWantedMIDICaptureChannels;
        fParams.fReturnMidiChannels = fWantedMIDIPlaybackChannels;
        
        fParams.fSlaveSyncMode = fEngineControl->fSyncMode;

        // Display some additional infos
        jack_info("NetDriver started in %s mode %s Master's transport sync.",
                    (fParams.fSlaveSyncMode) ? "sync" : "async", (fParams.fTransportSync) ? "with" : "without");

        // Init network
        if (!JackNetSlaveInterface::Init()) {
            jack_error("Starting network fails...");
            return false;
        }

        // Set global parameters
        if (!SetParams()) {
            jack_error("SetParams error...");
            return false;
        }

        // If -1 at connection time for audio, in/out audio channels count is sent by the master
        fCaptureChannels = fParams.fSendAudioChannels;
        fPlaybackChannels = fParams.fReturnAudioChannels;
        
        // If -1 at connection time for MIDI, in/out MIDI channels count is sent by the master (in fParams struct)
   
        // Allocate midi ports lists
        delete[] fMidiCapturePortList;
        delete[] fMidiPlaybackPortList;
        
        if (fParams.fSendMidiChannels > 0) {
            fMidiCapturePortList = new jack_port_id_t [fParams.fSendMidiChannels];
            assert(fMidiCapturePortList);
            for (int midi_port_index = 0; midi_port_index < fParams.fSendMidiChannels; midi_port_index++) {
                fMidiCapturePortList[midi_port_index] = 0;
            }
        }
        
        if (fParams.fReturnMidiChannels > 0) {
            fMidiPlaybackPortList = new jack_port_id_t [fParams.fReturnMidiChannels];
            assert(fMidiPlaybackPortList);
            for (int midi_port_index = 0; midi_port_index < fParams.fReturnMidiChannels; midi_port_index++) {
                fMidiPlaybackPortList[midi_port_index] = 0;
            }
        }

        // Register jack ports
        if (AllocPorts() != 0) {
            jack_error("Can't allocate ports.");
            return false;
        }

        // Init done, display parameters
        SessionParamsDisplay(&fParams);

        // Monitor
#ifdef JACK_MONITOR
        string plot_name;
        // NetTimeMon
        plot_name = string(fParams.fName);
        plot_name += string("_slave");
        plot_name += (fEngineControl->fSyncMode) ? string("_sync") : string("_async");
        plot_name += string("_latency");
        fNetTimeMon = new JackGnuPlotMonitor<float>(128, 5, plot_name);
        string net_time_mon_fields[] =
        {
            string("sync decoded"),
            string("end of read"),
            string("start of write"),
            string("sync send"),
            string("end of write")
        };
        string net_time_mon_options[] =
        {
            string("set xlabel \"audio cycles\""),
            string("set ylabel \"% of audio cycle\"")
        };
        fNetTimeMon->SetPlotFile(net_time_mon_options, 2, net_time_mon_fields, 5);
#endif
        // Driver parametering
        JackTimedDriver::SetBufferSize(fParams.fPeriodSize);
        JackTimedDriver::SetSampleRate(fParams.fSampleRate);

        JackDriver::NotifyBufferSize(fParams.fPeriodSize);
        JackDriver::NotifySampleRate(fParams.fSampleRate);

        // Transport engine parametering
        fEngineControl->fTransport.SetNetworkSync(fParams.fTransportSync);

        if (fAutoSave) {
            LoadConnections(0);
        }
        return true;
    }

    void JackNetDriver::FreeAll()
    {
        FreePorts();

        delete[] fTxBuffer;
        delete[] fRxBuffer;
        delete fNetAudioCaptureBuffer;
        delete fNetAudioPlaybackBuffer;
        delete fNetMidiCaptureBuffer;
        delete fNetMidiPlaybackBuffer;
        delete[] fMidiCapturePortList;
        delete[] fMidiPlaybackPortList;

        fTxBuffer = NULL;
        fRxBuffer = NULL;
        fNetAudioCaptureBuffer = NULL;
        fNetAudioPlaybackBuffer = NULL;
        fNetMidiCaptureBuffer = NULL;
        fNetMidiPlaybackBuffer = NULL;
        fMidiCapturePortList = NULL;
        fMidiPlaybackPortList = NULL;

#ifdef JACK_MONITOR
        delete fNetTimeMon;
        fNetTimeMon = NULL;
#endif
    }
    
    void JackNetDriver::UpdateLatencies()
    {
        jack_latency_range_t input_range;
        jack_latency_range_t output_range;
        jack_latency_range_t monitor_range;
     
        for (int i = 0; i < fCaptureChannels; i++) {
            input_range.max = input_range.min = float(fParams.fNetworkLatency * fEngineControl->fBufferSize) / 2.f;
            fGraphManager->GetPort(fCapturePortList[i])->SetLatencyRange(JackCaptureLatency, &input_range);
        }

        for (int i = 0; i < fPlaybackChannels; i++) {
            output_range.max = output_range.min = float(fParams.fNetworkLatency * fEngineControl->fBufferSize) / 2.f;
            if (!fEngineControl->fSyncMode) {
                output_range.max = output_range.min += fEngineControl->fBufferSize;
            }
            fGraphManager->GetPort(fPlaybackPortList[i])->SetLatencyRange(JackPlaybackLatency, &output_range);
            if (fWithMonitorPorts) {
                monitor_range.min = monitor_range.max = 0;
                fGraphManager->GetPort(fMonitorPortList[i])->SetLatencyRange(JackCaptureLatency, &monitor_range);
            }
        }
    }

//jack ports and buffers--------------------------------------------------------------
    int JackNetDriver::AllocPorts()
    {
        jack_log("JackNetDriver::AllocPorts fBufferSize = %ld fSampleRate = %ld", fEngineControl->fBufferSize, fEngineControl->fSampleRate);

        /*
            fNetAudioCaptureBuffer                fNetAudioPlaybackBuffer
            fSendAudioChannels                    fReturnAudioChannels

            fCapturePortList                      fPlaybackPortList
            fCaptureChannels    ==> SLAVE ==>     fPlaybackChannels
            "capture_"                            "playback_"
        */

        JackPort* port;
        jack_port_id_t port_index;
        char name[REAL_JACK_PORT_NAME_SIZE+1];
        char alias[REAL_JACK_PORT_NAME_SIZE+1];
        int audio_port_index;
        int midi_port_index;
     
        //audio
        for (audio_port_index = 0; audio_port_index < fCaptureChannels; audio_port_index++) {
            snprintf(alias, sizeof(alias), "%s:%s:out%d", fAliasName, fCaptureDriverName, audio_port_index + 1);
            snprintf(name, sizeof(name), "%s:capture_%d", fClientControl.fName, audio_port_index + 1);
            if (fEngine->PortRegister(fClientControl.fRefNum, name, JACK_DEFAULT_AUDIO_TYPE,
                             CaptureDriverFlags, fEngineControl->fBufferSize, &port_index) < 0) {
                jack_error("driver: cannot register port for %s", name);
                return -1;
            }

            port = fGraphManager->GetPort(port_index);
            port->SetAlias(alias);
            fCapturePortList[audio_port_index] = port_index;
            jack_log("JackNetDriver::AllocPorts() fCapturePortList[%d] audio_port_index = %ld fPortLatency = %ld", audio_port_index, port_index, port->GetLatency());
        }

        for (audio_port_index = 0; audio_port_index < fPlaybackChannels; audio_port_index++) {
            snprintf(alias, sizeof(alias), "%s:%s:in%d", fAliasName, fPlaybackDriverName, audio_port_index + 1);
            snprintf(name, sizeof(name), "%s:playback_%d",fClientControl.fName, audio_port_index + 1);
            if (fEngine->PortRegister(fClientControl.fRefNum, name, JACK_DEFAULT_AUDIO_TYPE,
                             PlaybackDriverFlags, fEngineControl->fBufferSize, &port_index) < 0) {
                jack_error("driver: cannot register port for %s", name);
                return -1;
            }

            port = fGraphManager->GetPort(port_index);
            port->SetAlias(alias);
            fPlaybackPortList[audio_port_index] = port_index;
            jack_log("JackNetDriver::AllocPorts() fPlaybackPortList[%d] audio_port_index = %ld fPortLatency = %ld", audio_port_index, port_index, port->GetLatency());
        }

        //midi
        for (midi_port_index = 0; midi_port_index < fParams.fSendMidiChannels; midi_port_index++) {
            snprintf(alias, sizeof(alias), "%s:%s:out%d", fAliasName, fCaptureDriverName, midi_port_index + 1);
            snprintf(name, sizeof (name), "%s:midi_capture_%d", fClientControl.fName, midi_port_index + 1);
            if (fEngine->PortRegister(fClientControl.fRefNum, name, JACK_DEFAULT_MIDI_TYPE,
                             CaptureDriverFlags, fEngineControl->fBufferSize, &port_index) < 0) {
                jack_error("driver: cannot register port for %s", name);
                return -1;
            }

            port = fGraphManager->GetPort(port_index);
            fMidiCapturePortList[midi_port_index] = port_index;
            jack_log("JackNetDriver::AllocPorts() fMidiCapturePortList[%d] midi_port_index = %ld fPortLatency = %ld", midi_port_index, port_index, port->GetLatency());
        }

        for (midi_port_index = 0; midi_port_index < fParams.fReturnMidiChannels; midi_port_index++) {
            snprintf(alias, sizeof(alias), "%s:%s:in%d", fAliasName, fPlaybackDriverName, midi_port_index + 1);
            snprintf(name, sizeof(name), "%s:midi_playback_%d", fClientControl.fName, midi_port_index + 1);
            if (fEngine->PortRegister(fClientControl.fRefNum, name, JACK_DEFAULT_MIDI_TYPE,
                             PlaybackDriverFlags, fEngineControl->fBufferSize, &port_index) < 0) {
                jack_error("driver: cannot register port for %s", name);
                return -1;
            }

            port = fGraphManager->GetPort(port_index);
            fMidiPlaybackPortList[midi_port_index] = port_index;
            jack_log("JackNetDriver::AllocPorts() fMidiPlaybackPortList[%d] midi_port_index = %ld fPortLatency = %ld", midi_port_index, port_index, port->GetLatency());
        }

        UpdateLatencies();
        return 0;
    }

    int JackNetDriver::FreePorts()
    {
        jack_log("JackNetDriver::FreePorts");

        for (int audio_port_index = 0; audio_port_index < fCaptureChannels; audio_port_index++) {
            if (fCapturePortList[audio_port_index] > 0) {
                fEngine->PortUnRegister(fClientControl.fRefNum, fCapturePortList[audio_port_index]);
                fCapturePortList[audio_port_index] = 0;
            }
        }

        for (int audio_port_index = 0; audio_port_index < fPlaybackChannels; audio_port_index++) {
            if (fPlaybackPortList[audio_port_index] > 0) {
                fEngine->PortUnRegister(fClientControl.fRefNum, fPlaybackPortList[audio_port_index]);
                fPlaybackPortList[audio_port_index] = 0;
            }
        }

        for (int midi_port_index = 0; midi_port_index < fParams.fSendMidiChannels; midi_port_index++) {
            if (fMidiCapturePortList && fMidiCapturePortList[midi_port_index] > 0) {
                fGraphManager->ReleasePort(fClientControl.fRefNum, fMidiCapturePortList[midi_port_index]);
                fMidiCapturePortList[midi_port_index] = 0;
            }
        }

        for (int midi_port_index = 0; midi_port_index < fParams.fReturnMidiChannels; midi_port_index++) {
            if (fMidiPlaybackPortList && fMidiPlaybackPortList[midi_port_index] > 0) {
                fEngine->PortUnRegister(fClientControl.fRefNum, fMidiPlaybackPortList[midi_port_index]);
                fMidiPlaybackPortList[midi_port_index] = 0;
            }
        }
        return 0;
    }

    void JackNetDriver::SaveConnections(int alias)
    {
        JackDriver::SaveConnections(alias);
        const char** connections;

        if (fMidiCapturePortList) {
            for (int i = 0; i < fParams.fSendMidiChannels; ++i) {
                if (fMidiCapturePortList[i] && (connections = fGraphManager->GetConnections(fMidiCapturePortList[i])) != 0) {
                    for (int j = 0; connections[j]; j++) {
                        JackPort* port_id = fGraphManager->GetPort(fGraphManager->GetPort(connections[j]));
                        fConnections.push_back(make_pair(port_id->GetType(), make_pair(fGraphManager->GetPort(fMidiCapturePortList[i])->GetName(), connections[j])));
                        jack_info("Save connection: %s %s", fGraphManager->GetPort(fMidiCapturePortList[i])->GetName(), connections[j]);
                    }
                    free(connections);
                }
            }
        }

        if (fMidiPlaybackPortList) {
            for (int i = 0; i < fParams.fReturnMidiChannels; ++i) {
                if (fMidiPlaybackPortList[i] && (connections = fGraphManager->GetConnections(fMidiPlaybackPortList[i])) != 0) {
                    for (int j = 0; connections[j]; j++) {
                        JackPort* port_id = fGraphManager->GetPort(fGraphManager->GetPort(connections[j]));
                        fConnections.push_back(make_pair(port_id->GetType(), make_pair(connections[j], fGraphManager->GetPort(fMidiPlaybackPortList[i])->GetName())));
                        jack_info("Save connection: %s %s", connections[j], fGraphManager->GetPort(fMidiPlaybackPortList[i])->GetName());
                    }
                    free(connections);
                }
            }
        }
    }

    JackMidiBuffer* JackNetDriver::GetMidiInputBuffer(int port_index)
    {
        return static_cast<JackMidiBuffer*>(fGraphManager->GetBuffer(fMidiCapturePortList[port_index], fEngineControl->fBufferSize));
    }

    JackMidiBuffer* JackNetDriver::GetMidiOutputBuffer(int port_index)
    {
        return static_cast<JackMidiBuffer*>(fGraphManager->GetBuffer(fMidiPlaybackPortList[port_index], fEngineControl->fBufferSize));
    }

//transport---------------------------------------------------------------------------
    void JackNetDriver::DecodeTransportData()
    {
        //is there a new timebase master on the net master ?
        // - release timebase master only if it's a non-conditional request
        // - no change or no request : don't do anything
        // - conditional request : don't change anything too, the master will know if this slave is actually the timebase master
        int refnum;
        bool conditional;
        if (fSendTransportData.fTimebaseMaster == TIMEBASEMASTER) {
            fEngineControl->fTransport.GetTimebaseMaster(refnum, conditional);
            if (refnum != -1) {
                fEngineControl->fTransport.ResetTimebase(refnum);
            }
            jack_info("The NetMaster is now the new timebase master.");
        }

        //is there a transport state change to handle ?
        if (fSendTransportData.fNewState &&(fSendTransportData.fState != fEngineControl->fTransport.GetState())) {

            switch (fSendTransportData.fState)
            {
                case JackTransportStopped :
                    fEngineControl->fTransport.SetCommand(TransportCommandStop);
                    jack_info("Master stops transport.");
                    break;

                case JackTransportStarting :
                    fEngineControl->fTransport.RequestNewPos(&fSendTransportData.fPosition);
                    fEngineControl->fTransport.SetCommand(TransportCommandStart);
                    jack_info("Master starts transport frame = %d", fSendTransportData.fPosition.frame);
                    break;

                case JackTransportRolling :
                    //fEngineControl->fTransport.SetCommand(TransportCommandStart);
                    fEngineControl->fTransport.SetState(JackTransportRolling);
                    jack_info("Master is rolling.");
                    break;
            }
        }
    }

    void JackNetDriver::EncodeTransportData()
    {
        // is there a timebase master change ?
        int refnum;
        bool conditional;
        fEngineControl->fTransport.GetTimebaseMaster(refnum, conditional);
        if (refnum != fLastTimebaseMaster) {
            // timebase master has released its function
            if (refnum == -1) {
                fReturnTransportData.fTimebaseMaster = RELEASE_TIMEBASEMASTER;
                jack_info("Sending a timebase master release request.");
            } else {
                // there is a new timebase master
                fReturnTransportData.fTimebaseMaster = (conditional) ? CONDITIONAL_TIMEBASEMASTER : TIMEBASEMASTER;
                jack_info("Sending a %s timebase master request.", (conditional) ? "conditional" : "non-conditional");
            }
            fLastTimebaseMaster = refnum;
        } else {
            fReturnTransportData.fTimebaseMaster = NO_CHANGE;
        }

        // update transport state and position
        fReturnTransportData.fState = fEngineControl->fTransport.Query(&fReturnTransportData.fPosition);

        // is it a new state (that the master need to know...) ?
        fReturnTransportData.fNewState = ((fReturnTransportData.fState == JackTransportNetStarting) &&
                                           (fReturnTransportData.fState != fLastTransportState) &&
                                           (fReturnTransportData.fState != fSendTransportData.fState));
        if (fReturnTransportData.fNewState) {
            jack_info("Sending '%s'.", GetTransportState(fReturnTransportData.fState));
        }
        fLastTransportState = fReturnTransportData.fState;
    }

//driver processes--------------------------------------------------------------------

    int JackNetDriver::Read()
    {
        // buffers
        for (int midi_port_index = 0; midi_port_index < fParams.fSendMidiChannels; midi_port_index++) {
            fNetMidiCaptureBuffer->SetBuffer(midi_port_index, GetMidiInputBuffer(midi_port_index));
        }

        for (int audio_port_index = 0; audio_port_index < fParams.fSendAudioChannels; audio_port_index++) {
        #ifdef OPTIMIZED_PROTOCOL
            if (fGraphManager->GetConnectionsNum(fCapturePortList[audio_port_index]) > 0) {
                fNetAudioCaptureBuffer->SetBuffer(audio_port_index, GetInputBuffer(audio_port_index));
            } else {
                fNetAudioCaptureBuffer->SetBuffer(audio_port_index, NULL);
            }
        #else
            fNetAudioCaptureBuffer->SetBuffer(audio_port_index, GetInputBuffer(audio_port_index));
        #endif
        }

#ifdef JACK_MONITOR
        fNetTimeMon->New();
#endif

        switch (SyncRecv()) {
        
            case SOCKET_ERROR:
                return SOCKET_ERROR;
                
            case SYNC_PACKET_ERROR:
                // since sync packet is incorrect, don't decode it and continue with data
                break;
                
            default:
                // decode sync
                int unused_frames;
                DecodeSyncPacket(unused_frames);
                break;
        }
  
#ifdef JACK_MONITOR
        // For timing
        fRcvSyncUst = GetMicroSeconds();
#endif

#ifdef JACK_MONITOR
        fNetTimeMon->Add(float(GetMicroSeconds() - fRcvSyncUst) / float(fEngineControl->fPeriodUsecs) * 100.f);
#endif
        // audio, midi or sync if driver is late
        switch (DataRecv()) {
        
            case SOCKET_ERROR:
                return SOCKET_ERROR;
                
            case DATA_PACKET_ERROR:
                jack_time_t cur_time = GetMicroSeconds();
                NotifyXRun(cur_time, float(cur_time - fBeginDateUst));  // Better this value than nothing...
                break;
        }
 
        // take the time at the beginning of the cycle
        JackDriver::CycleTakeBeginTime();

#ifdef JACK_MONITOR
        fNetTimeMon->Add(float(GetMicroSeconds() - fRcvSyncUst) / float(fEngineControl->fPeriodUsecs) * 100.f);
#endif

        return 0;
    }

    int JackNetDriver::Write()
    {
        // buffers
        for (int midi_port_index = 0; midi_port_index < fParams.fReturnMidiChannels; midi_port_index++) {
            fNetMidiPlaybackBuffer->SetBuffer(midi_port_index, GetMidiOutputBuffer(midi_port_index));
        }

        for (int audio_port_index = 0; audio_port_index < fPlaybackChannels; audio_port_index++) {
        #ifdef OPTIMIZED_PROTOCOL
            // Port is connected on other side...
            if (fNetAudioPlaybackBuffer->GetConnected(audio_port_index)
                && (fGraphManager->GetConnectionsNum(fPlaybackPortList[audio_port_index]) > 0)) {
                fNetAudioPlaybackBuffer->SetBuffer(audio_port_index, GetOutputBuffer(audio_port_index));
            } else {
                fNetAudioPlaybackBuffer->SetBuffer(audio_port_index, NULL);
            }
        #else
            fNetAudioPlaybackBuffer->SetBuffer(audio_port_index, GetOutputBuffer(audio_port_index));
        #endif
        }

#ifdef JACK_MONITOR
        fNetTimeMon->AddLast(float(GetMicroSeconds() - fRcvSyncUst) / float(fEngineControl->fPeriodUsecs) * 100.f);
#endif

        EncodeSyncPacket();

        // send sync
        if (SyncSend() == SOCKET_ERROR) {
            return SOCKET_ERROR;
        }

#ifdef JACK_MONITOR
        fNetTimeMon->Add(((float)(GetMicroSeconds() - fRcvSyncUst) / (float)fEngineControl->fPeriodUsecs) * 100.f);
#endif

        // send data
        if (DataSend() == SOCKET_ERROR) {
            return SOCKET_ERROR;
        }

#ifdef JACK_MONITOR
        fNetTimeMon->AddLast(((float)(GetMicroSeconds() - fRcvSyncUst) / (float)fEngineControl->fPeriodUsecs) * 100.f);
#endif

        return 0;
    }

//driver loader-----------------------------------------------------------------------

#ifdef __cplusplus
    extern "C"
    {
#endif

        SERVER_EXPORT jack_driver_desc_t* driver_get_descriptor()
        {
            jack_driver_desc_t * desc;
            jack_driver_desc_filler_t filler;
            jack_driver_param_value_t value;

            desc = jack_driver_descriptor_construct("net", JackDriverMaster, "netjack slave backend component", &filler);

            strcpy(value.str, DEFAULT_MULTICAST_IP);
            jack_driver_descriptor_add_parameter(desc, &filler, "multicast-ip", 'a', JackDriverParamString, &value, NULL, "Multicast address, or explicit IP of the master", NULL);

            strcpy(value.str, DEFAULT_MULTICAST_IF);
            jack_driver_descriptor_add_parameter(desc, &filler, "multicast-if", 'f', JackDriverParamString, &value, NULL, "Multicast interface name or any", "Multicast interface to send probes from (any - kernel chosen (default), if_name - send from specified interface)");

            value.i = DEFAULT_PORT;
            jack_driver_descriptor_add_parameter(desc, &filler, "udp-net-port", 'p', JackDriverParamInt, &value, NULL, "UDP port", NULL);

            value.i = DEFAULT_MTU;
            jack_driver_descriptor_add_parameter(desc, &filler, "mtu", 'M', JackDriverParamInt, &value, NULL, "MTU to the master", NULL);

            value.i = -1;
            jack_driver_descriptor_add_parameter(desc, &filler, "input-ports", 'C', JackDriverParamInt, &value, NULL, "Number of audio input ports", "Number of audio input ports. If -1, audio physical input from the master");
            jack_driver_descriptor_add_parameter(desc, &filler, "output-ports", 'P', JackDriverParamInt, &value, NULL, "Number of audio output ports", "Number of audio output ports. If -1, audio physical output from the master");

            value.i = -1;
            jack_driver_descriptor_add_parameter(desc, &filler, "midi-in-ports", 'i', JackDriverParamInt, &value, NULL, "Number of midi input ports", "Number of MIDI input ports. If -1, MIDI physical input from the master");
            jack_driver_descriptor_add_parameter(desc, &filler, "midi-out-ports", 'o', JackDriverParamInt, &value, NULL, "Number of midi output ports", "Number of MIDI output ports. If -1, MIDI physical output from the master");

#if HAVE_CELT
            value.i = -1;
            jack_driver_descriptor_add_parameter(desc, &filler, "celt", 'c', JackDriverParamInt, &value, NULL, "Set CELT encoding and number of kBits per channel", NULL);
#endif
#if HAVE_OPUS
            value.i = -1;
            jack_driver_descriptor_add_parameter(desc, &filler, "opus", 'O', JackDriverParamInt, &value, NULL, "Set Opus encoding and number of kBits per channel", NULL);
#endif
            strcpy(value.str, "'hostname'");
            jack_driver_descriptor_add_parameter(desc, &filler, "client-name", 'n', JackDriverParamString, &value, NULL, "Name of the jack client", NULL);
            
            value.i = false;
            jack_driver_descriptor_add_parameter(desc, &filler, "auto-save", 's', JackDriverParamBool, &value, NULL, "Save/restore connection state when restarting", NULL);


/*  
Deactivated for now..
            value.ui = 0U;
            jack_driver_descriptor_add_parameter(desc, &filler, "transport-sync", 't', JackDriverParamUInt, &value, NULL, "Sync transport with master's", NULL);
*/

            value.ui = 5U;
            jack_driver_descriptor_add_parameter(desc, &filler, "latency", 'l', JackDriverParamUInt, &value, NULL, "Network latency", NULL);

            return desc;
        }

        SERVER_EXPORT Jack::JackDriverClientInterface* driver_initialize(Jack::JackLockedEngine* engine, Jack::JackSynchro* table, const JSList* params)
        {
            char multicast_if[32];
            char multicast_ip[32];
            char net_name[JACK_CLIENT_NAME_SIZE + 1] = {0};
            int udp_port;
            int mtu = DEFAULT_MTU;
            // Deactivated for now...
            uint transport_sync = 0;
            jack_nframes_t period_size = 1024;  // to be used while waiting for master period_size
            jack_nframes_t sample_rate = 48000; // to be used while waiting for master sample_rate
            int audio_capture_ports = -1;
            int audio_playback_ports = -1;
            int midi_input_ports = -1;
            int midi_output_ports = -1;
            int celt_encoding = -1;
            int opus_encoding = -1;
            bool monitor = false;
            int network_latency = 5;
            const JSList* node;
            const jack_driver_param_t* param;
            bool auto_save = false;

            // Possibly use env variable for UDP port
            const char* default_udp_port = getenv("JACK_NETJACK_PORT");
            udp_port = (default_udp_port) ? atoi(default_udp_port) : DEFAULT_PORT;

            // Possibly use env variable for multicast IP
            const char* default_multicast_ip = getenv("JACK_NETJACK_MULTICAST");
            strcpy(multicast_ip, (default_multicast_ip) ? default_multicast_ip : DEFAULT_MULTICAST_IP);
         
            const char* default_multicast_if = getenv("JACK_NETJACK_INTERFACE");
            strcpy(multicast_if, (default_multicast_if) ? default_multicast_if : DEFAULT_MULTICAST_IF);

            for (node = params; node; node = jack_slist_next(node)) {
                param = (const jack_driver_param_t*) node->data;
                switch (param->character)
                {
                    case 'a' :
                        assert(strlen(param->value.str) < 32);
                        strcpy(multicast_ip, param->value.str);
                        break;
                    case 'f' :
                        assert(strlen(param->value.str) < 32);
                        strcpy(multicast_if, param->value.str);
                        break;
                    case 'p':
                        udp_port = param->value.ui;
                        break;
                    case 'M':
                        mtu = param->value.i;
                        break;
                    case 'C':
                        audio_capture_ports = param->value.i;
                        break;
                    case 'P':
                        audio_playback_ports = param->value.i;
                        break;
                    case 'i':
                        midi_input_ports = param->value.i;
                        break;
                    case 'o':
                        midi_output_ports = param->value.i;
                        break;
                    #if HAVE_CELT
                    case 'c':
                        celt_encoding = param->value.i;
                        break;
                    #endif
                    #if HAVE_OPUS
                    case 'O':
                        opus_encoding = param->value.i;
                        break;
                    #endif
                    case 'n' :
                        strncpy(net_name, param->value.str, JACK_CLIENT_NAME_SIZE);
                        break;
                    case 's':
                        auto_save = true;
                        break;
                    /*
                    Deactivated for now..
                    case 't' :
                        transport_sync = param->value.ui;
                        break;
                    */
                    case 'l' :
                        network_latency = param->value.ui;
                        if (network_latency > NETWORK_MAX_LATENCY) {
                            printf("Error : network latency is limited to %d\n", NETWORK_MAX_LATENCY);
                            return NULL;
                        }
                        break;
                }
            }

            try {

                Jack::JackDriverClientInterface* driver = new Jack::JackWaitThreadedDriver(
                        new Jack::JackNetDriver("system", "net_pcm", engine, table, multicast_ip, udp_port, multicast_if, mtu,
                                                midi_input_ports, midi_output_ports,
                                                net_name, transport_sync,
                                                network_latency, celt_encoding, opus_encoding, auto_save));
                if (driver->Open(period_size, sample_rate, 1, 1, audio_capture_ports, audio_playback_ports, monitor, "from_master_", "to_master_", 0, 0) == 0) {
                    return driver;
                } else {
                    delete driver;
                    return NULL;
                }

            } catch (...) {
                return NULL;
            }
        }

#ifdef __cplusplus
    }
#endif
}
