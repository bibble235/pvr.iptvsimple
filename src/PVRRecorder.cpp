/*
 *      Copyright (C) 2015 Radek Kubera
 *      http://github.com/afedchin/xbmc-addon-iptvsimple/
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */
#include <map>
#include <vector>
#include <string>
#include <time.h>

#include "platform/util/StringUtils.h"
#include "platform/threads/threads.h"
#include "PVRIptvData.h"
#include "PVRRecorder.h"

using namespace ADDON;
using namespace std;
extern PVRIptvData   *m_data;
string p_recordingsPath;
string p_ffmpegPath;
string p_ffmpegParams;
string p_rtmpdumpPath;
PVRRecJob *p_RecJob;
PVRSchedulerThread *p_Scheduler;

PVRRecorder::PVRRecorder (PVRIptvData *data, const std::string &recordingsPath, const std::string &ffmpegPath, const std::string &ffmpegParams, const std::string &rtmpdumpPath)
{
    m_data = data;
    p_recordingsPath = recordingsPath;
    p_ffmpegPath = ffmpegPath;
    p_ffmpegParams = ffmpegParams;
    p_rtmpdumpPath = rtmpdumpPath;
    p_RecJob = new PVRRecJob (data);
    p_Scheduler = new PVRSchedulerThread();
}

PVR_ERROR PVRRecorder::GetTimers(ADDON_HANDLE handle)
{
    PVRIptvChannel currentChannel;
    p_RecJob->setLock();
    
    map<int,PVR_REC_JOB_ENTRY> RecJobs = p_RecJob->getEntryData();
    for (map<int,PVR_REC_JOB_ENTRY>::iterator rec=RecJobs.begin(); rec!=RecJobs.end(); ++rec)
    {
        if (p_RecJob->getProperlyChannel(rec->second,currentChannel))
        {
            p_RecJob->updateJobEntry(rec->second);
            PVR->TransferTimerEntry(handle, &rec->second.Timer); 
        }
    }
    p_RecJob->setUnlock();
    return PVR_ERROR_NO_ERROR;
}

int PVRRecorder::GetTimersAmount(void)
{
    p_RecJob->setLock();
    
    map<int,PVR_REC_JOB_ENTRY> RecJobs = p_RecJob->getEntryData();
    int size = RecJobs.size();
    
    p_RecJob->setUnlock();
    return size;
}

PVR_ERROR PVRRecorder::UpdateTimer(const PVR_TIMER &timer)
{
    PVR_TIMER myTimer = timer;
    PVR_REC_JOB_ENTRY entry;
    if (p_RecJob->getJobEntry(timer.iClientIndex,entry))
    {
        
        if (entry.Timer.state==PVR_TIMER_STATE_RECORDING && (entry.Status==PVR_STREAM_IS_RECORDING || entry.Status==PVR_STREAM_START_RECORDING || entry.Status==PVR_STREAM_IS_STOPPING)) {
            //Job is active
            if (timer.state==PVR_TIMER_STATE_CANCELLED)
            {
               //stop recording
               p_Scheduler->stopRecording(entry);
            }
            else
            {
                myTimer.state = entry.Timer.state;
            }
        }
        entry.Timer = myTimer;
        p_RecJob->updateJobEntry(entry);
        PVR->TriggerTimerUpdate();
        return PVR_ERROR_NO_ERROR;
    }
    PVR->TriggerTimerUpdate();
    return PVR_ERROR_FAILED;
}

PVR_ERROR PVRRecorder::DeleteTimer(const PVR_TIMER &timer, bool bForceDelete)
{    
    PVR_REC_JOB_ENTRY entry;
    if (p_RecJob->getJobEntry(timer.iClientIndex,entry))
    {
        if (p_Scheduler->stopRecording(entry))
        {
            PVR->TriggerTimerUpdate();
            return PVR_ERROR_NO_ERROR;
        }
    }
    return PVR_ERROR_FAILED;
}

PVR_ERROR PVRRecorder::AddTimer (const PVR_TIMER &timer)
{
    if (p_recordingsPath.length()==0)
    {
        XBMC->Log(LOG_ERROR,"Store folder is not set. Please change addon configuration.");
        return PVR_ERROR_FAILED;
    }
    
    //Set start time for Job
    time_t startTime = timer.startTime;
    //Bad start time and end time  
    if (timer.startTime>=timer.endTime)
        return PVR_ERROR_FAILED;

    //Correct start time if needed
    if (timer.startTime == 0 || timer.startTime<time(NULL))
      startTime = time(NULL);
    
    //GetChannel definition
    PVR_CHANNEL channel;
    channel.iUniqueId = timer.iClientChannelUid;
    
    PVRIptvChannel currentChannel;
    
    if (!m_data->GetChannel(channel, currentChannel))
        return PVR_ERROR_FAILED;
    
    //set end time for Job
    time_t endTime = timer.endTime;
    /*
    if (timer.startTime==0)
    {     
        EPG_TAG tag;
        PVR_ERROR myData = m_data->GetEPGTagForChannel(tag,channel, startTime, startTime);
        if (myData == PVR_ERROR_NO_ERROR)
            endTime = tag.endTime;
    }
    */
    p_RecJob->setLock();
    
    //Check, if job is aleready schduled for this time
    map<int,PVR_REC_JOB_ENTRY> RecJobs = p_RecJob->getEntryData();
    for (map<int,PVR_REC_JOB_ENTRY>::iterator rec=RecJobs.begin(); rec!=RecJobs.end(); ++rec)
    {
        if (rec->second.strChannelName==currentChannel.strChannelName)
        {
            if ((rec->second.Timer.endTime>startTime && rec->second.Timer.startTime<endTime)
                    || (rec->second.Timer.startTime<endTime && rec->second.Timer.endTime>startTime)
                    || (rec->second.Timer.startTime<startTime && rec->second.Timer.endTime>endTime)
                    || (rec->second.Timer.startTime>startTime && rec->second.Timer.endTime<endTime)) 
            {
                //Similar recording already scheduled
                if ((rec->second.Timer.state==PVR_TIMER_STATE_SCHEDULED || (rec->second.Timer.state==PVR_TIMER_STATE_RECORDING)) && (rec->second.Status==PVR_STREAM_IS_RECORDING || rec->second.Status==PVR_STREAM_START_RECORDING))
                {
                    //Send message to refresh timers
                    p_RecJob->setUnlock();
                    return PVR_ERROR_ALREADY_PRESENT;
                }
            }
        }
    }
    
    p_RecJob->setUnlock();
    
    //recalculate new job id
    int iJobId = timer.iClientIndex;
    if (iJobId==-1)
        iJobId = time(NULL);
        
    //Prepare new entry
    PVR_REC_JOB_ENTRY recJobEntry;
    recJobEntry.Timer = timer;
    recJobEntry.Timer.iClientIndex = iJobId;
    recJobEntry.Timer.startTime = startTime;
    recJobEntry.Timer.endTime = endTime;
    recJobEntry.strChannelName = currentChannel.strChannelName;
    p_RecJob->addJobEntry(recJobEntry);
    
    if (startTime<=time(NULL))
    {
        recJobEntry.Status = PVR_STREAM_IS_RECORDING;
        recJobEntry.Timer.state = PVR_TIMER_STATE_RECORDING;
        p_RecJob->updateJobEntry(recJobEntry);
        p_Scheduler->startRecording(recJobEntry);
        
    }
    else
    {
        recJobEntry.Status = PVR_STREAM_NO_STREAM;
        recJobEntry.Timer.state = PVR_TIMER_STATE_SCHEDULED;
        p_RecJob->updateJobEntry(recJobEntry);
    }
    PVR->TriggerTimerUpdate();
    return PVR_ERROR_NO_ERROR;
}