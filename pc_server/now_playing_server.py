
import asyncio
import struct
import time

import websockets

from concurrent.futures import ThreadPoolExecutor # Used to help with the COM issues between pycaw and DataReader, I don't know it works :)

from datetime import datetime

from websockets import ConnectionClosed
from winrt.windows.media.control import GlobalSystemMediaTransportControlsSessionManager, GlobalSystemMediaTransportControlsSessionPlaybackStatus
from winrt.windows.storage.streams import DataReader

import json

from PIL import Image

#Volume functions
audio_pool = ThreadPoolExecutor(max_workers=1)  # one thread, so COM state stays consistent

UPDATE_MANAGER_TIME = 5 #constant used to check if the media has switched platforms

startTime = 0
avgTimes = []

backgroundColor = 0xFFFFFF

setup_received = asyncio.Event()

DISPLAYTYPE = -1 # 1 is standard, 2 is album disk

CHARACTER_REPLACEMENTS = {
    # Apostrophes and quotation marks
    "\u02BC": "'",   # ʼ modifier letter apostrophe
    "\u2018": "'",   # ‘ left single quote
    "\u2019": "'",   # ’ right single quote
    "\u201B": "'",   # ‛ reversed single quote
    "\u02BB": "'",   # ʻ modifier letter turned comma
    "\u201C": '"',   # “ left double quote
    "\u201D": '"',   # ” right double quote
    "\u201F": '"',   # ‟ double high-reversed quote

    # Dashes and hyphens
    "\u2010": "-",   # ‐ hyphen
    "\u2011": "-",   # ‑ nonbreaking hyphen
    "\u2012": "-",   # ‒ figure dash
    "\u2013": "-",   # – en dash
    "\u2014": "-",   # — em dash
    "\u2212": "-",   # − minus sign

    # Dots and ellipses
    "\u2026": "...", # … ellipsis
    "\u2024": ".",   # ․ one dot leader

    # Spaces
    "\u00A0": " ",   # nonbreaking space
    "\u2002": " ",   # en space
    "\u2003": " ",   # em space
    "\u2009": " ",   # thin space
    "\u202F": " ",   # narrow nonbreaking space
    "\u3000": " ",   # ideographic space

    # Common music symbols
    "\u2665": "<3",  # ♥ heart
    "\u266A": "",    # ♪ music note
    "\u266B": "",    # ♫ double music note
    "\u2605": "*",   # ★ black star
    "\u2606": "*",   # ☆ white star
    "\u2022": "*",   # • bullet
    "\u00A9": "(c)", # © copyright
    "\u00AE": "(R)", # ® registered
    "\u2122": "(TM)",# ™ trademark

    # Common accented letters (ASCII approximations)
    "á": "a", "à": "a", "â": "a", "ä": "a",
    "å": "a", "Á": "A", "À": "A", "Â": "A", "Ä": "A",
    "ã": "a", "æ": "ae", "Æ": "AE",
    "é": "e", "è": "e", "ê": "e", "ë": "e",
    "É": "E", "È": "E", "Ê": "E", "Ë": "E",
    "í": "i", "ì": "i", "î": "i", "ï": "i",
    "Í": "I", "Ì": "I", "Î": "I", "Ï": "I",
    "ó": "o", "ò": "o", "ô": "o", "ö": "o", "õ": "o",
    "Ó": "O", "Ò": "O", "Ô": "O", "Ö": "O", "Õ": "O",
    "ú": "u", "ù": "u", "û": "u", "ü": "u",
    "Ú": "U", "Ù": "U", "Û": "U", "Ü": "U",
    "ñ": "n", "Ñ": "N",
    "ç": "c", "Ç": "C",
    "ý": "y", "ÿ": "y", "Ý": "Y",
    "ß": "ss",
} # the font used with LVGL will not print certain characters

# pycaw functions set up for async
def _set_volume(level):  # level 0.0 to 1.0
    from pycaw.pycaw import AudioUtilities
    for s in AudioUtilities.GetAllSessions():
        s.SimpleAudioVolume.SetMasterVolume(level, None)

def _set_mute(mute):  # sets all muted (true or false
    from pycaw.pycaw import AudioUtilities
    for s in AudioUtilities.GetAllSessions():
        s.SimpleAudioVolume.SetMute(1 if mute else 0, None)

def _get_volume():
    from pycaw.pycaw import AudioUtilities

    devices = AudioUtilities.GetSpeakers()
    volume = devices.EndpointVolume

    return round(volume.GetMasterVolumeLevelScalar() * 100)

# pycaw functions converted to async
async def mute(isMuted):  # use me
    loop = asyncio.get_running_loop()
    await loop.run_in_executor(audio_pool, _set_mute, not isMuted)

async def setVolume(level):  # use me
    loop = asyncio.get_running_loop()
    await loop.run_in_executor(audio_pool, _set_volume, level)

async def getVolume():
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(audio_pool, _get_volume)

def fixName(name):
    newName = ""
    for char in name:
        if char in CHARACTER_REPLACEMENTS:
            newName += CHARACTER_REPLACEMENTS[char]
            continue

        newName+= char

    return newName


async def createThumbnail(thumbnail_ref):
    stream = await asyncio.wait_for(thumbnail_ref.open_read_async(), timeout=5)

    reader = DataReader(stream)

    await asyncio.wait_for(reader.load_async(stream.size), timeout=5)

    data = bytearray(stream.size)
    reader.read_bytes(data)

    with open("albumCover.png", "wb") as file:
        file.write(data)

    reader.close()
    stream.close()

    print("Streams closed")

    with Image.open("albumCover.png") as img:
        cover = img.copy()

    print("Image loaded")

    cover.thumbnail((140, 140), Image.Resampling.LANCZOS) # it does NOT like 240 by 240
    print("Image resized:", cover.size)

    cover.save("AlbumCoverForESP.png", format="PNG")


async def sendThumbnail(websocket, filepath, output_path):
    await setup_received.wait()

    CHUNK_SIZE = 8192

    img = Image.open(filepath).convert("RGB")

    width, height = img.size

    response = {
        "type": "imageSpecs",
        "height": height,
        "width": width
    }

    print("Display type:", DISPLAYTYPE)

    await websocket.send(json.dumps(response))
    if DISPLAYTYPE == 1 or height != width:
        with open(output_path, "wb") as f:
            # Loop through pixels row by row
            for y in range(height):
                for x in range(width):
                    r, g, b = img.getpixel((x, y))

                    # Convert 8-bit channels to 5-6-5 bits
                    r_5 = (r >> 3) & 0x1F  # Keep top 5 bits
                    g_6 = (g >> 2) & 0x3F  # Keep top 6 bits
                    b_5 = (b >> 3) & 0x1F  # Keep top 5 bits

                    # Combine into a single 16-bit integer
                    # Format: RRRRRGGG GGGBBBBB
                    rgb565 = (r_5 << 11) | (g_6 << 5) | b_5

                    f.write(struct.pack("<H", rgb565)) #"<H" for little endian
    elif DISPLAYTYPE == 2:
        print("printing with rim")
        midpoint = height/2 # radius = width = height
        print("Combined RGB565:", hex(backgroundColor))
        with open(output_path, "wb") as f:
            # Loop through pixels row by row
            for y in range(height):
                for x in range(width):
                    xDist = (x - midpoint)**2
                    yDist = (y - midpoint)**2
                    if xDist + yDist <= midpoint**2:
                        r, g, b = img.getpixel((x, y))

                        # Convert 8-bit channels to 5-6-5 bits
                        r_5 = (r >> 3) & 0x1F  # Keep top 5 bits
                        g_6 = (g >> 2) & 0x3F  # Keep top 6 bits
                        b_5 = (b >> 3) & 0x1F  # Keep top 5 bits

                        # Combine into a single 16-bit integer
                        # Format: RRRRRGGG GGGBBBBB
                        rgb565 = (r_5 << 11) | (g_6 << 5) | b_5

                        f.write(struct.pack("<H", rgb565))  # "<H" for little endian
                    else:
                        f.write(struct.pack("<H", backgroundColor))  # "<H" for little endian
    else:
        print("Neither")


    with open(output_path, "rb") as f:
        imageData = f.read()

    for i in range(0, len(imageData), CHUNK_SIZE):
        chunk = imageData[i:i + CHUNK_SIZE]
        await websocket.send(chunk)
        print(f"Sent chunk at offset {i}: {len(chunk)} bytes")


def getPositionOffset(timeLineProperties):
    lastUpdate = timeLineProperties.last_updated_time

    currentTime = datetime.now(lastUpdate.tzinfo)

    return currentTime - lastUpdate

async def waitForSession(mediaController):
    while True:
        session = mediaController.get_current_session()
        print("Waiting for session...")
        if session is not None:
            return session

        await asyncio.sleep(1)

async def handle_client(websocket): #set up websocket
    print("ESP32 connected!")
    received_setup = set()
    #initialize the first manager and states
    # ask windows for the controller
    mediaController = await GlobalSystemMediaTransportControlsSessionManager.request_async()

    # ask if the media player is currently active
    session = await waitForSession(mediaController)

    try:
        asyncio.create_task(update_media(mediaController, session, websocket))
    except ConnectionClosed as e:
        print(f"WebSocket disconnected: {e}")
        return

    try:
        async for message in websocket:
            if isinstance(message, str):
                try:
                    data = json.loads(message)
                    if "action" in data:
                        if data["action"] == "SpeedTest":
                            global avgTimes
                            diff = time.perf_counter() - startTime
                            print("Time between there and back:", diff)
                            if diff < 1000: #spikes to some unreasonable number at the beginning when initializing, skips it
                                avgTimes += [diff]
                                print("avg: ", sum(avgTimes) / len(avgTimes))
                    if "BGColor" in data:
                        global backgroundColor

                        BGCOLOR = data["BGColor"]
                        r = (BGCOLOR >> 16) & 0xFF
                        g = (BGCOLOR >> 8) & 0xFF
                        b = BGCOLOR & 0xFF

                        backgroundColor = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                        print("BGColor ", BGCOLOR)
                        received_setup.add("BGColor")
                    if "DisplayType" in data:
                        global DISPLAYTYPE
                        DISPLAYTYPE = data["DisplayType"]
                        print("Display type", DISPLAYTYPE)
                        received_setup.add("DisplayType")
                    if {"BGColor", "DisplayType"}.issubset(received_setup):
                        if not setup_received.is_set():
                            print("Setup complete. Image transfer can begin.")
                            setup_received.set()
                except json.JSONDecodeError:
                    print("Message Received: ", message)

    except ConnectionClosed:
        print("ESP32 disconnected.")


async def update_media(mediaController, session, websocket):  #main media grabber
    if session is not None: # first intialization
        # initial setup
        mediaProperties = await session.try_get_media_properties_async()
        timeLineProperties = session.get_timeline_properties()
        playbackProperties = session.get_playback_info()

        isShuffling = playbackProperties.is_shuffle_active
        STARTTIME = timeLineProperties.start_time

        currentSong = mediaProperties.title
        currentArtist = mediaProperties.artist
        currentAlbum = mediaProperties.album_title
        # reference current song

        thumbnail_ref = mediaProperties.thumbnail  # pulls the reference

        if thumbnail_ref:
            await createThumbnail(thumbnail_ref)
            await sendThumbnail(websocket,"AlbumCoverForESP.png", "albumCoverRGB.rgb565")


        endTime = timeLineProperties.end_time  # gets total time
        totalTimePaused = 0  # this is going to check if the user switched media players
        lastPosition = timeLineProperties.position
        while True:
            session = mediaController.get_current_session()
            if session is not None:
                # get updated info
                mediaProperties = await session.try_get_media_properties_async()
                timeLineProperties = session.get_timeline_properties()
                playbackProperties = session.get_playback_info()
                if currentSong != mediaProperties.title:  # are we on the same song
                    endTime = timeLineProperties.end_time
                    currentSong = mediaProperties.title
                    currentAlbum = mediaProperties.album_title
                    currentArtist = mediaProperties.artist
                    thumbnail_ref = mediaProperties.thumbnail
                    if thumbnail_ref:
                        await createThumbnail(thumbnail_ref)
                        await sendThumbnail(websocket,"AlbumCoverForESP.png", "albumCoverRGB.rgb565")

                currentVol = await getVolume()
                position = timeLineProperties.position  # get current spot in the song
                offset = getPositionOffset(
                    timeLineProperties)  # compensate for delayed windows update -> compare last update to current time
                isPlaying = playbackProperties.playback_status == GlobalSystemMediaTransportControlsSessionPlaybackStatus.PLAYING
                if isPlaying:
                    position = position + offset

                # if lastPosition == position: #old way of checking if the session changed
                #     totalTimePaused = totalTimePaused + 1
                # else:
                #     totalTimePaused = 0
                #
                # if totalTimePaused >= UPDATE_MANAGER_TIME:
                #     session = mediaController.get_current_session()
                #     totalTimePaused = 0

                posInSeconds = int(position.total_seconds())
                endTimeInSeconds = int(endTime.total_seconds())

                global startTime
                startTime = time.perf_counter()
                response = {
                    "type": "media_update",
                    "volume": currentVol,
                    "playing": isPlaying,
                    "title": fixName(currentSong),
                    "artist": fixName(currentArtist),
                    "album": currentAlbum,
                    "currentPos": posInSeconds,
                    "endTime": endTimeInSeconds,
                }

                # print(response)
                try:
                    await websocket.send(json.dumps(response))
                except ConnectionClosed as e:
                    print(f"WebSocket disconnected: {e}")
                    return
            else:
                print(f"Session was closed\nChecking for new session")
                response = {
                    "type": "media_update",
                    "volume": 0,
                    "playing": False,
                    "title": "Unknown",
                    "artist": "No Artist Available",
                    "album": "N/A",
                    "currentPos": 0,
                    "endTime": 0,
                }
                try:
                    await websocket.send(json.dumps(response))
                except ConnectionClosed as e:
                    print(f"WebSocket disconnected: {e}")
                    return
            await asyncio.sleep(1)

#Functions^^




async def main():
    async with websockets.serve(handle_client,host = "0.0.0.0",port = 8765, max_size = 2 * 1024 * 1024): #handle web socket connections
        print("WebSocket server running on port 8765")
        print("Waiting for ESP32...")
        await asyncio.Future() #keeps main running

if __name__ == "__main__":
    asyncio.run(main())