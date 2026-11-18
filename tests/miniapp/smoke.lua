-- Verification app; layout follows the device's actual canvas.
local count, ticks = 0, 0
local http_status, tts_status = "idle", "idle"
local http_id, speech_id
local storage_status = "unread"
local last_second = -1

local function draw()
    if not screen then return end
    local w, h = app.width, app.height
    screen.begin(0x101820, {ordered=true})
    screen.rect(0,0,w,4,0x42c7a5)
    screen.text("Lua miniapp",12,12,0xffffff)
    screen.text(string.format("%d x %d  clicks: %d",w,h,count),12,42,0x70cfff)
    screen.circle(w-36,32,14,0x42c7a5,{fill=false,border_width=3})
    screen.arc(w-36,32,10,270,90,0xffcc66,{width=3,rounded=true})
    screen.line(-8,68,w+8,68,0x56616a,{width=2})
    screen.text("HTTP: "..http_status,12,82,0xffffff,{width=w-24})
    screen.text("TTS: "..tts_status,12,110,0xffffff,{width=w-24})
    screen.text("Audio: "..(audio and audio.get_state() or "unavailable"),12,138,0xffffff)
    screen.text("Store: "..storage_status,12,166,0xffffff)
    screen.text("Click to count; hold to exit",12,h-28,0xa5b5c5,{width=w-24})
    screen.present()
end
function on_start()
    local saved = storage.load()
    if saved then count=saved.count or 0;storage_status="restored" else storage_status="empty" end
    draw()
end
function on_tick(dt)
    ticks=ticks+1
    local now=clock.now()
    if now and now~=last_second then last_second=now;draw() end
end
function on_button_click(id)
    count=count+1
    draw()
end
function on_http_response(id,response)
    assert(id==http_id,"stale HTTP result")
    http_id=nil
    if response.error then http_status=response.error.code
    else
        local decoded=json.decode(response.body)
        http_status=tostring(response.status)..":"..(type(decoded)=="table" and tostring(decoded.value) or "text")
    end
    draw()
end
function on_tts_result(id,result)
    assert(id==speech_id,"stale TTS result")
    speech_id=nil;tts_status=result.status..(result.error and ":"..result.error.code or "")
    draw()
end
function on_audio_state(state) draw() end
function get_state()
    return json.encode({count=count,ticks=ticks,http=http_status,tts=tts_status,
        audio=audio and audio.get_state() or "unavailable",storage=storage_status,
        clock=clock.now() or 0,width=app.width,height=app.height})
end
function get_actions()
    return {
        set_count={description="设置计数",parameters={value={type="integer",required=true,minimum=0,maximum=999}}},
        fetch={description="请求测试 JSON",parameters={url={type="string",required=true}}},
        speak={description="播报测试文本",parameters={text={type="string",required=true}}},
        play={description="播放在线音频",parameters={url={type="string",required=true}}},
        pause={description="暂停在线音频",parameters={}},
        resume={description="恢复在线音频",parameters={}},
        stop={description="停止在线音频",parameters={}},
        sound={description="播放四声部音效",parameters={}},
        synth_pause={description="暂停音效",parameters={}},
        synth_resume={description="恢复音效",parameters={}},
        save={description="保存计数",parameters={}}
    }
end
function on_action(name,args)
    if name=="set_count" then count=args.value
    elseif name=="fetch" then http_id=http.get(args.url);http_status="pending"
    elseif name=="speak" then speech_id=tts.speak(args.text);tts_status="pending"
    elseif name=="play" then if not audio.play(args.url) then return {ok=false,message="audio busy"} end
    elseif name=="pause" then audio.pause()
    elseif name=="resume" then audio.resume()
    elseif name=="stop" then audio.stop()
    elseif name=="synth_pause" then synth.pause()
    elseif name=="synth_resume" then synth.resume()
    elseif name=="sound" then
        local notes={}
        for v=1,4 do notes[v]={at=0,ms=1200,hz=220*v,voice=v,volume=35,wave=v==4 and "noise" or "triangle"} end
        if not synth.play(notes) then return {ok=false,message="synth busy"} end
    elseif name=="save" then
        local ok,reason=storage.save({count=count})
        storage_status=ok and "saved" or reason
    else return {ok=false,message="unknown action"} end
    draw()
    return {ok=true,message=get_state()}
end
