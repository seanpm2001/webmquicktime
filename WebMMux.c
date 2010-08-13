// Copyright (c) 2010 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.



#include <QuickTime/QuickTime.h>
#include "WebMExportStructs.h"
//#include "debug.h"

#include "EbmlIDs.h"
#include "log.h"
#include "WebMAudioStream.h"
#include "WebMMux.h"

#define kVorbisPrivateMaxSize  4000
#define kSInt16Max 32768

static ComponentResult _updateProgressBar(WebMExportGlobalsPtr globals, double percent);

static UInt64 secondsToTimeCode(WebMExportGlobalsPtr globals, double timeInSeconds)
{
    UInt64 rval = 0;

    if (globals->webmTimeCodeScale == 0)
    {
        dbg_printf("[webm] ERROR= division by 0 globals->webmTimeCodeScale\n");
        return 0;
    }

    rval = timeInSeconds * 1000000000 / globals->webmTimeCodeScale;
    return rval;
}

static double getMaxDuration(WebMExportGlobalsPtr globals)
{
    int i;
    double duration = 0.0;
    double dtmp = 0.0;
    TimeRecord durationTimeRec;

    // loop over all the data sources and find the max duration
    for (i = 0; i < globals->streamCount; i++)
    {
        GenericStream *gs = &(*globals->streams)[i];
        StreamSource *source;

        if (gs->trackType == VideoMediaType)
            source = &gs->stream.vid.source;
        else
            source = &gs->stream.aud.source;

        // get the track duration if it is available
        if (InvokeMovieExportGetPropertyUPP(source->refCon, source->trackID,
                                            movieExportDuration,
                                            &durationTimeRec,
                                            source->propertyProc) == noErr)
        {
            dtmp = (double) durationTimeRec.value.lo / (double) durationTimeRec.scale;
            dbg_printf("[webm] track duration # %d = %f\n", i, dtmp);

            if (duration < dtmp)
                duration = dtmp;
        }
    }

    return duration;
}


static ComponentResult _writeTracks(WebMExportGlobalsPtr globals, EbmlGlobal *ebml, EbmlLoc* trackStart)
{
    ComponentResult err = noErr;
    int i;
    {
        Ebml_StartSubElement(ebml, trackStart, Tracks);

        // Write tracks
        for (i = 0; i < globals->streamCount; i++)
        {
            ComponentResult gErr = noErr;
            GenericStream *gs = &(*globals->streams)[i];
            dbg_printf("[WebM] Write track %d\n", i);

            if (gs->trackType == VideoMediaType)
            {
                VideoStreamPtr vs = &gs->stream.vid;
                double fps = FixedToFloat(globals->movie_fps);
                if (fps == 0)
                {
                    //framerate estime should be replaced with more accurate
                    fps = vs->source.timeScale / 100.0;
                }
                err = InvokeMovieExportGetDataUPP(vs->source.refCon, &vs->source.params,
                                                  vs->source.dataProc);
                ImageDescription *id = *(ImageDescriptionHandle) vs->source.params.desc;

                dbg_printf("[webM] write vid track #%d : %dx%d  %f fps\n",
                           vs->source.trackID, id->width, id->height, fps);
                writeVideoTrack(ebml, vs->source.trackID,
                                0, /*flag lacing*/
                                "V_VP8", id->width, id->height, fps);
            }
            else if (gs->trackType == SoundMediaType)
            {
                AudioStreamPtr as = &gs->stream.aud;
                unsigned int trackNumber;
                double sampleRate = 0;
                unsigned int channels = 0;

                if (as->vorbisComponentInstance == NULL)
                {
                    //Here I am setting the input properties for this component
                    err = initVorbisComponent(globals, as);

                    if (err) return err;

                    sampleRate = as->asbd.mSampleRate;
                    channels = as->asbd.mChannelsPerFrame;
                }

                UInt8 *privateData = NULL;
                UInt32 privateDataSize = 0;
                write_vorbisPrivateData(as, &privateData, &privateDataSize);
                dbg_printf("[WebM] Writing audio track %d with %d bytes private data, %d channels, %d sampleRate\n",
                           as->source.trackID, privateDataSize, channels, sampleRate);
                writeAudioTrack(ebml, as->source.trackID, 0 /*no lacing*/, "A_VORBIS" /*fixed for now*/,
                                sampleRate, channels, privateData, privateDataSize);
                dbg_printf("[WebM] finished audio write \n");

                if (privateData != NULL)
                    free(privateData);
            }
        }

        Ebml_EndSubElement(ebml, trackStart);
    }
    dbg_printf("[webM] exit write trakcs = %d\n", err);
    return err;
}


static ComponentResult _updateProgressBar(WebMExportGlobalsPtr globals, double percent)
{
    ComponentResult err = noErr;

    if (globals->progressOpen == false)
    {
        InvokeMovieProgressUPP(NULL, movieProgressOpen,
                               progressOpExportMovie, 0,
                               globals->progressRefCon,
                               globals->progressProc);
        globals->progressOpen = true;
    }

    Fixed percentDone = FloatToFixed(percent);

    if (globals->progressProc)
    {

        if (percentDone > 0x010000)
            percentDone = 0x010000;

        err = InvokeMovieProgressUPP(NULL, movieProgressUpdatePercent,
                                     progressOpExportMovie, percentDone,
                                     globals->progressRefCon,
                                     globals->progressProc);
    }

    if (percentDone == 100.0 && globals->progressOpen)
    {
        InvokeMovieProgressUPP(NULL, movieProgressClose,
                               progressOpExportMovie, 0x010000,
                               globals->progressRefCon,
                               globals->progressProc);
        globals->progressOpen == false;
    }

    return err;
}

static void _writeSeekElement(EbmlGlobal* ebml, unsigned long binaryId, UInt64 Loc)
{
    EbmlLoc start;
    Ebml_StartSubElement(ebml, &start, Seek);
    Ebml_WriteBinary(ebml, SeekID, binaryId);
    Ebml_SerializeUnsigned64(ebml, SeekPosition, Loc);
    Ebml_EndSubElement(ebml, &start);
}

static void _writeMetaSeekInformation(EbmlGlobal *ebml, UInt64 trackLoc, UInt64 cueLoc,  
                                      UInt64 clusterLoc, EbmlLoc* seekInfoLoc, Boolean firstWrite)
{
    EbmlLoc globLoc;
    if (firstWrite)
    {
        Ebml_StartSubElement(ebml, seekInfoLoc, SeekHead);
    }
    else 
    {
        Ebml_GetEbmlLoc(ebml, &globLoc);
        Ebml_SetEbmlLoc(ebml, seekInfoLoc);
    }
    
    _writeSeekElement(ebml, Tracks, trackLoc);
    _writeSeekElement(ebml, Cues, cueLoc);
    _writeSeekElement(ebml, Cluster, clusterLoc);
    
    if (firstWrite)
        Ebml_EndSubElement(ebml, seekInfoLoc);
    else
        Ebml_SetEbmlLoc(ebml, &globLoc);
}

static void _writeCues(WebMExportGlobalsPtr globals, EbmlGlobal *ebml, EbmlLoc *cuesLoc)
{
    dbg_printf("[webm]_writeCues %d \n", globals->cueCount);
    Ebml_StartSubElement(ebml, cuesLoc, Cues);
    int i = 0;

    for (i = 0; i < globals->cueCount; i ++)
    {
        EbmlLoc cueHead;
        WebMCuePoint *cue = &(*globals->cueHandle)[i];
        dbg_printf("[WebM] Writing Cue track %d time %ld loc %lld\n",
                   cue->track, cue->timeVal, cue->loc);
        Ebml_StartSubElement(ebml, &cueHead, CuePoint);
        Ebml_SerializeUnsigned(ebml, CueTime, cue->timeVal);

        EbmlLoc trackLoc;
        Ebml_StartSubElement(ebml, &trackLoc, CueTrackPositions);
        //TODO this is wrong, get the conversion right
        Ebml_SerializeUnsigned(ebml, CueTrack, cue->track);
        Ebml_SerializeUnsigned64(ebml, CueClusterPosition, cue->loc);
        Ebml_SerializeUnsigned(ebml, CueBlockNumber, 1);
        Ebml_EndSubElement(ebml, &trackLoc);

        Ebml_EndSubElement(ebml, &cueHead);
    }

    Ebml_EndSubElement(ebml, cuesLoc);
}

void _addCue(WebMExportGlobalsPtr globals, UInt64 dataLoc, unsigned long time, 
             unsigned int track, unsigned int blockNum)
{
    dbg_printf("[webm] _addCue time %ld loc %llu\n", time, dataLoc);
    globals->cueCount ++;

    if (globals->cueHandle)
        SetHandleSize((Handle) globals->cueHandle, sizeof(CuePoint) * globals->cueCount);
    else
        globals->cueHandle = (WebMCuePoint **) NewHandleClear(sizeof(WebMCuePoint));

    WebMCuePoint *newCue = &(*globals->cueHandle)[globals->cueCount-1];
    newCue->loc = dataLoc;
    newCue->timeVal = time;
    newCue->track = track;
    newCue->blockNumber = blockNum;
}
static ComponentResult _compressVideo(WebMExportGlobalsPtr globals, VideoStreamPtr vs)
{
    ComponentResult err = noErr;

    if (vs->source.bQdFrame || vs->source.eos)
        return err; //paranoid check

    dbg_printf("[webM] call Compress Next frame %d\n", vs->currentFrame);
    // get next frame as vp8 frame
    err = compressNextFrame(globals, vs);

    if (err != noErr)
    {
        dbg_printf("[webM] compressNextFrame error %d\n", err);
    }

    if (!vs->source.eos)
        vs->source.bQdFrame = true;

    return err;
}

static void _startNewCluster(WebMExportGlobalsPtr globals, EbmlGlobal *ebml)
{
    dbg_printf("[webm] Starting new cluster at %ld\n", globals->clusterTime);
    if (globals->clusterTime != 0)  //case of: first cluster (don't end non-existant previous)
        Ebml_EndSubElement(ebml, &globals->clusterStart);
    
    Ebml_StartSubElement(ebml, &globals->clusterStart, Cluster);
    Ebml_SerializeUnsigned(ebml, Timecode, globals->clusterTime);
}

static ComponentResult _writeVideo(WebMExportGlobalsPtr globals, VideoStreamPtr vs, EbmlGlobal *ebml)
{
    ComponentResult err = noErr;
    StreamSource *source =  &vs->source;
    unsigned long lastTime = source->blockTimeMs;
    int isKeyFrame = vs->frame_type == kICMFrameType_I;
    dbg_printf("[webM] video write simple block track %d keyframe %d frame #%ld time %d data size %ld\n",
               source->trackID, isKeyFrame,
               vs->currentFrame, lastTime, vs->outBuf.size);
    unsigned long relativeTime = lastTime - globals->clusterTime;


    writeSimpleBlock(ebml, source->trackID, (short)relativeTime,
                     isKeyFrame, 0 /*unsigned char lacingFlag*/, 0/*int discardable*/,
                     vs->outBuf.data, vs->outBuf.size);
    vs->source.bQdFrame = false;
    
    //this now represents the next frame we want to encode
    double fps = FixedToFloat(globals->movie_fps);
    if (fps == 0)
        fps = source->params.sourceTimeScale * 1.0/ source->params.durationPerSample * 1.0; 
    vs->currentFrame += 1;  
    source->time = (SInt32)((vs->currentFrame * 1.0) / fps * source->timeScale); //TODO  -- I am assuming that each frame has a similar fps
        /*source->time += source->params.durationPerSample * source->timeScale
            / source->params.sourceTimeScale;  //TODO precision loss??*/

    source->blockTimeMs = getTimeAsSeconds(source) * 1000;
    dbg_printf("[WebM] Next frame calculated %f from %f fps, durationPerSample %ld * timeScale %d / sourceTimeScale %d to %d \n"
               , getTimeAsSeconds(source),fps,  source->params.durationPerSample ,source->timeScale
               , source->params.sourceTimeScale, source->time);
    
    return err;
}

static ComponentResult _compressAudio(AudioStreamPtr as)
{
    ComponentResult err = noErr;

    if (as->source.bQdFrame)
        return err; //paranoid check

    err = compressAudio(as);

    if (err) return err;


    if (!as->source.eos)
        as->source.bQdFrame = true;

    return err;
}

static ComponentResult _writeAudio(WebMExportGlobalsPtr globals, AudioStreamPtr as, EbmlGlobal *ebml)
{
    ComponentResult err = noErr;
    unsigned long lastTime = as->source.blockTimeMs;
    unsigned long relativeTime = lastTime - globals->clusterTime;
    dbg_printf("[WebM] writing %d size audio packet with relative time %d, packet time %d input stream time %f\n",
               as->outBuf.offset, relativeTime, lastTime, getTimeAsSeconds(&as->source));

    writeSimpleBlock(ebml, as->source.trackID, (short)relativeTime,
                     1 /*audio always key*/, 0 /*unsigned char lacingFlag*/, 0/*int discardable*/,
                     as->outBuf.data, as->outBuf.offset);
    double timeSeconds = (1.0 * as->currentEncodedFrames) / (1.0 * as->asbd.mSampleRate);
    as->source.blockTimeMs = (SInt32)(timeSeconds * 1000);
    
    dbg_printf("[webm] _compressAudio new audio time %f %d %s\n",
               getTimeAsSeconds(&as->source), as->source.blockTimeMs, as->source.eos ? "eos" : "");
    
    as->source.bQdFrame = false;
    return err;
}

ComponentResult muxStreams(WebMExportGlobalsPtr globals, DataHandler data_h)
{
    ComponentResult err = noErr;
    double duration = getMaxDuration(globals);
    dbg_printf("[WebM-%08lx] :: muxStreams( duration %f)\n", (UInt32) globals, duration);
    
    UInt32 iStream;
    Boolean allStreamsDone = false;

    //initialize my ebml writing structure
    EbmlGlobal ebml;
    ebml.data_h = data_h;
    ebml.offset.hi = 0;
    ebml.offset.lo = 0;

    EbmlLoc startSegment, trackStart, cuesLoc;
    globals->progressOpen = false;
	
	writeHeader(&ebml);    
    dbg_printf("[WebM]) Write segment information\n");
    Ebml_StartSubElement(&ebml, &startSegment, Segment);
	SInt64 firstL1Offset = *(SInt64*) &ebml.offset;  //The first level 1 element is the offset needed for cuepoints according to Matroska's specs
    writeSegmentInformation(&ebml, globals->webmTimeCodeScale, duration);  

    _writeTracks(globals, &ebml, &trackStart);    

    Boolean bExportVideo = globals->bMovieHasVideo && globals->bExportVideo;
    Boolean bExportAudio = globals->bMovieHasAudio && globals->bExportAudio;

    HLock((Handle)globals->streams);
    err = _updateProgressBar(globals, 0.0);

    unsigned long minTimeMs = ULONG_MAX;
    GenericStream *minTimeStream;

    globals->clusterTime = 0;  //assuming 0 start time
    Boolean startNewCluster = true;  //cluster should start very first
    unsigned int blocksInCluster=0;  //this increments any time a block added
    while (!allStreamsDone /*&& lastTime < duration*/)
    {
        minTimeMs = ULONG_MAX;
        minTimeStream = NULL;
        allStreamsDone = true;
        SInt64 blockOffset = *(SInt64 *)& ebml.offset;
        
        dbg_printf("[WebM]          ebml.offset  %lld\n", blockOffset);

        //find the stream with the earliest time
        for (iStream = 0; iStream < globals->streamCount; iStream++)
        {
            GenericStream *gs = &(*globals->streams)[iStream];
            StreamSource *source;

            if (gs->trackType == VideoMediaType)
            {
                source =  &gs->stream.vid.source;

                if (!source->bQdFrame && globals->bExportVideo)
                {
                    err = _compressVideo(globals, &gs->stream.vid);
                    //I need to know if there's a video keyframe in the Queue
                    if (!startNewCluster)
                        startNewCluster = gs->stream.vid.frame_type == kICMFrameType_I;
                }
            }

            if (gs->trackType == SoundMediaType)
            {
                source = &gs->stream.aud.source;

                if (!source->bQdFrame && globals->bExportAudio)
                    err = _compressAudio(&gs->stream.aud);
            }

            if (err)
            {
                dbg_printf("[webm] _compress error = %d\n", err);
                goto bail;
            }
			
            Boolean smallerTime = false;
            if (gs->trackType == VideoMediaType)
                smallerTime = source->blockTimeMs < minTimeMs;
            else if (gs->trackType == SoundMediaType)
                smallerTime = source->blockTimeMs <= minTimeMs; //similar time audio first (see webm specs)									
			
            if (smallerTime && source->bQdFrame && !err)
            {
                minTimeMs = source->blockTimeMs;
                minTimeStream = gs;
                allStreamsDone = false;
            }
        }  //end for loop

        
        //write the stream with the earliest time
        if (minTimeStream == NULL)
            break;
        
        
        dbg_printf("[Webm] Stream with smallest time %d(ms)  %s: start Cluster %d\n",
                   minTimeMs, minTimeStream->trackType == VideoMediaType ? "video" : "audio", startNewCluster);

        if (minTimeMs - globals->clusterTime > 32767)
            startNewCluster = true; //keep in mind the block time offset to the cluster is SInt16
        
        if (startNewCluster)
        {
            globals->clusterTime = minTimeMs;
            blocksInCluster =0;
            _startNewCluster(globals, &ebml);
            startNewCluster = false;
        }

        
        if (minTimeStream->trackType == VideoMediaType)
        {
            VideoStreamPtr vs = &minTimeStream->stream.vid;
            _writeVideo(globals, vs, &ebml);
            blocksInCluster ++;            
            if( vs->frame_type == kICMFrameType_I)
            {
                UInt64 tmpU = blockOffset - firstL1Offset;  
                _addCue(globals, tmpU , globals->clusterTime, vs->source.trackID, blocksInCluster);
            }

        }  //end if VideoMediaType
        else if (minTimeStream->trackType == SoundMediaType)
        {
            AudioStreamPtr as = &minTimeStream->stream.aud;
            _writeAudio(globals, as, &ebml);
            blocksInCluster ++;
        } //end SoundMediaType

        Ebml_EndSubElement(&ebml, &globals->clusterStart);   //this writes cluster size multiple times, but works

        if (duration != 0.0)  //if duration is 0, can't show anything
            _updateProgressBar(globals, minTimeMs / 1000.0 / duration );
    }

    dbg_printf("[webm] done writing streams\n");
    _writeCues(globals, &ebml, &cuesLoc);
    Ebml_EndSubElement(&ebml, &startSegment);

    HUnlock((Handle) globals->streams);

    _updateProgressBar(globals, 100.0);
bail:
    dbg_printf("[WebM] <   [%08lx] :: muxStreams() = %ld\n", (UInt32) globals, err);
    return err;
}
