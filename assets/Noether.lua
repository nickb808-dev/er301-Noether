-- SPDX-License-Identifier: Apache-2.0
-- Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
--
-- Noether — seamless vari-speed loop recorder.
-- One REC button (empty -> record -> play -> overdub in -> overdub out), a
-- phase-matched seam, continuous Speed from -4x to +4x plus 1 V/oct, a
-- Start / Len playback window, Extend (overdub past the end grows the loop),
-- SOS crossfader, Dry / Level, and loops that are saved to the card with the
-- preset and come back with it. See README.md and LOOPER-PLAN.md.
--
-- EVERY app.* / self:* CALL HERE ALREADY APPEARS IN A SHIPPED UNIT'S LUA
-- (FeedbackLooper.lua, PedalLooper.lua, Dirac.lua, Landau.lua). Verify with
-- the comm(1) audit in ER301-UNIT-STANDARD.md §1 before every release.

local app = app
local Class = require "Base.Class"
local Unit = require "Unit"
local Encoder = require "Encoder"
local GainBias = require "Unit.ViewControl.GainBias"
local Gate = require "Unit.ViewControl.Gate"
local Pitch = require "Unit.ViewControl.Pitch"
local Signal = require "Signal"
local Path = require "Path"
local SamplePool = require "Sample.Pool"
local SamplePoolInterface = require "Sample.Pool.Interface"
local Task = require "Unit.MenuControl.Task"
local MenuHeader = require "Unit.MenuControl.Header"
local Overlay = require "Overlay"
local ply = app.SECTION_PLY

local libnoether = require "noether.libnoether"

-- THE RUNNING VERSION, ON SCREEN (standard §10c). tools/check-version.sh
-- fails the build if this drifts from the Makefile or toc.lua.
local VERSION = "0.3.0"

-- Default loop buffer length. Changed from the menu (10 / 30 / 60 s).
local kDefaultSecs = 30

local Noether = Class {}
Noether:include(Unit)

function Noether:init(args)
  args.title = "Noether"
  args.mnemonic = "Nt"
  Unit.init(self, args)
  self.bufferSecs = kDefaultSecs
  self.loopRevSaved = -1
  -- The pool announces load / save completion with this signal (the
  -- Landau pattern, standard §12b): weakRegister = no unsubscribe, no
  -- reference, a deleted unit is never called.
  Signal.weakRegister("sampleStatusChanged", self)
end

-- One CV-able parameter: a GainBias driving the engine port, a MinMax so the
-- dial knows its range, and a mono branch so it takes CV. (Dirac / Landau.)
local function param(self, head, name, port, bias)
  local p = self:addObject(name .. "Param", app.GainBias())
  local r = self:addObject(name .. "Range", app.MinMax())
  p:hardSet("Bias", bias)
  connect(p, "Out", r, "In")
  connect(p, "Out", head, port)
  self:addMonoBranch(name, p, "In", p, "Out")
end

function Noether:onLoadGraph(channelCount)
  local head = self:addObject("head", libnoether.Noether(channelCount))

  connect(self, "In1", head, "Left In")
  connect(head, "Left Out", self, "Out1")
  if channelCount > 1 then
    connect(self, "In2", head, "Right In")
    connect(head, "Right Out", self, "Out2")
  end

  -- REC: a trigger Comparator, the same object the stock Pedal Looper puts
  -- in front of its Record inlet (G-jack noise and hot-unplug filtered).
  local rec = self:addObject("rec", app.Comparator())
  connect(rec, "Out", head, "Rec")
  self:addMonoBranch("rec", rec, "In", rec, "Out")

  -- EXTEND: a latched gate (tap on, tap off), same idiom as Dirac's Hold.
  local extg = self:addObject("extg", app.Comparator())
  extg:setToggleMode()
  connect(extg, "Out", head, "Extend")
  self:addMonoBranch("ext", extg, "In", extg, "Out")

  -- 1 V/oct into speed: a ConstantOffset + the Pitch view control, exactly
  -- as Dirac and Landau do it (a GainBias would be the wrong control).
  local tune      = self:addObject("tune",      app.ConstantOffset())
  local tuneRange = self:addObject("tuneRange", app.MinMax())
  connect(tune, "Out", tuneRange, "In")
  connect(tune, "Out", head, "V/Oct")
  self:addMonoBranch("voct", tune, "In", tune, "Out")

  param(self, head, "speed", "Speed", 1.0)
  param(self, head, "start", "Start", 0.0)
  param(self, head, "len",   "Len",   1.0)
  param(self, head, "sos",   "SOS",   0.5)
  param(self, head, "dry",   "Dry",   1.0)
  param(self, head, "level", "Level", 1.0)

  -- The loop buffer: a pool buffer, created here so the unit records the
  -- moment it is added. Replaced by the menu (10 / 30 / 60 s) and by a
  -- preset load (deserialize), which unload this one.
  self:createBuffer(self.bufferSecs or kDefaultSecs)
end

-- ── buffer ──────────────────────────────────────────────────────────────
-- Same claim / release order as the stock Feedback Looper.
function Noether:setSample(sample)
  local old = self.sample
  if old then
    old:release(self)
    self.sample = nil
  end
  self.sample = sample
  if self.sample then
    self.sample:claim(self)
    self.objects.head:setSample(sample.pSample)
  else
    self.objects.head:setSample(nil)
  end
  -- A buffer we created and nobody else uses goes back to the pool.
  if old and old ~= sample and self.ownBuffer == old and not old:isShared() then
    SamplePool.unload(old)
  end
  if self.ownBuffer ~= sample then self.ownBuffer = nil end
  self:notifyControls("setSample", sample)
end

function Noether:createBuffer(secs)
  local sample, msg = SamplePool.create {
    root = "noether",
    channels = self.channelCount,
    secs = secs
  }
  if sample then
    self.bufferSecs = secs
    self:setSample(sample)
    self.ownBuffer = sample
  else
    Overlay.flashMainMessage("Buffer failed: %s", msg or "?")
  end
end

function Noether:doSetBufferSecs(secs)
  self:createBuffer(secs)
  Overlay.flashMainMessage("Loop buffer: %d s", secs)
end

function Noether:doClearLoop()
  self.objects.head:clearLoop()
  Overlay.flashMainMessage("Loop cleared.")
end

-- Hand the loop to another buffer in the pool (or save it from there).
function Noether:doAttachBufferFromPool()
  local chooser = SamplePoolInterface(self.loadInfo.id, "choose")
  chooser:setDefaultChannelCount(self.channelCount)
  chooser:highlight(self.sample)
  chooser:subscribe("done", function(sample)
    if sample then
      self:setSample(sample)
      Overlay.flashMainMessage("Attached buffer: %s", sample.name)
    end
  end)
  chooser:show()
end

function Noether:showSampleEditor()
  if self.sample then
    if self.sampleEditor == nil then
      local SampleEditor = require "Sample.Editor"
      self.sampleEditor = SampleEditor(self, self.objects.head)
      self.sampleEditor:setSample(self.sample)
      self.sampleEditor:setPointerLabel("R")
    end
    self.sampleEditor:show()
  else
    Overlay.flashMainMessage("No buffer attached.")
  end
end

-- ── loop persistence ────────────────────────────────────────────────────
-- A pool BUFFER comes back from a preset empty (the pool only persists its
-- size), which is how the stock loopers lose their loops. So at serialize
-- time the loop [0, L) is copied into a temporary pool buffer of exactly L
-- frames, renamed to a .wav path under ER-301/noether/, and handed to
-- SamplePool.save (non-interactive when the directory exists and the name
-- ends in .wav — read in Sample/Pool/init.lua). The preset carries the path;
-- deserialize loads it with SamplePool.load and, when the pool says it has
-- arrived, copies it into the loop buffer. Only re-saved when the loop
-- changed (the engine's loop revision counter).
local kLoopDir = app.roots.front .. "/ER-301/noether"

function Noether:saveLoopToCard()
  local head = self.objects.head
  local L = head:getLoopSamples()
  if L <= 0 then return end
  local export, msg = SamplePool.create {
    root = "noether-save",
    channels = self.channelCount,
    samples = L
  }
  if not export then
    app.logError("%s: could not create export buffer (%s)", self, msg or "?")
    return
  end
  if head:exportLoop(export.pSample) <= 0 then
    SamplePool.unload(export)
    return
  end
  Path.createAll(kLoopDir)
  local path = Path.join(kLoopDir, string.format("loop-%06x-%04x.wav",
                         math.random(0, 0xffffff), math.random(0, 0xffff)))
  SamplePool.rename(export, path)
  if SamplePool.save(export) then
    self.loopPath = path
    self.loopRevSaved = head:getLoopRev()
    self.savingSample = export
  else
    SamplePool.unload(export)
  end
end

function Noether:sampleStatusChanged(sample)
  if sample == nil then return end
  if sample == self.savingSample and not sample:isPending() then
    self.savingSample = nil
    if sample.userCount == 0 then SamplePool.unload(sample) end
  elseif sample == self.pendingLoop and not sample:isPending() then
    self.pendingLoop = nil
    local ok, n = pcall(function() return self.objects.head:importLoop(sample.pSample) end)
    if ok and n and n > 0 then
      self.loopRevSaved = self.objects.head:getLoopRev()
    else
      app.logError("%s: loop file did not import (%s)", self, tostring(n))
    end
    if sample.userCount == 0 then SamplePool.unload(sample) end
  end
end

function Noether:serialize()
  local t = Unit.serialize(self)
  if self.sample then
    t.sample = SamplePool.serializeSample(self.sample)
  end
  t.bufferSecs = self.bufferSecs
  local head = self.objects.head
  if head:getLoopSamples() > 0 then
    if self.loopPath == nil or head:getLoopRev() ~= self.loopRevSaved then
      self:saveLoopToCard()
    end
    t.loopPath = self.loopPath
  end
  return t
end

function Noether:deserialize(t)
  Unit.deserialize(self, t)
  if t.bufferSecs then self.bufferSecs = t.bufferSecs end
  if t.sample then
    local sample = SamplePool.deserializeSample(t.sample, self.chain)
    if sample then
      self:setSample(sample)
    else
      app.logError("%s:deserialize: failed to load sample.", self)
    end
  end
  if t.loopPath and Path.exists(t.loopPath) then
    local s = SamplePool.load(t.loopPath)
    if s then
      self.loopPath = t.loopPath
      self.pendingLoop = s
      if not s:isPending() then self:sampleStatusChanged(s) end
    end
  end
end

function Noether:onRemove()
  self:setSample(nil)
  Unit.onRemove(self)
end

-- ── menu ────────────────────────────────────────────────────────────────
-- Tasks, not OptionControls: creating a buffer allocates, which the audio
-- thread may never do (standard §2 / §3).
local menu = {
  "bufferHeader", "buf10", "buf30", "buf60",
  "loopHeader", "clearLoop", "attachExisting", "editBuffer",
}

function Noether:onShowMenu(objects, branches)
  local controls = {}
  controls.bufferHeader = MenuHeader { description = "Loop buffer  (Noether v" .. VERSION .. ")" }
  local cur = self.bufferSecs or kDefaultSecs
  local function bufTask(name, secs)
    local label = string.format("%d s", secs)
    controls[name] = Task {
      description = (cur == secs) and (label .. "  <") or label,
      task = function() self:doSetBufferSecs(secs) end,
    }
  end
  bufTask("buf10", 10)
  bufTask("buf30", 30)
  bufTask("buf60", 60)

  controls.loopHeader = MenuHeader { description = "Loop" }
  controls.clearLoop = Task { description = "Clear loop",
    task = function() self:doClearLoop() end }
  controls.attachExisting = Task { description = "Attach pool buffer...",
    task = function() self:doAttachBufferFromPool() end }
  controls.editBuffer = Task { description = "Edit / save buffer",
    task = function() self:showSampleEditor() end }

  local sub = {}
  if self.sample then
    sub[1] = { position = app.GRID5_LINE1, justify = app.justifyLeft, text = "Buffer:" }
    sub[2] = { position = app.GRID5_LINE2, justify = app.justifyLeft,
               text = self.sample.name or "attached" }
    sub[3] = { position = app.GRID5_LINE3, justify = app.justifyLeft,
               text = string.format("%d s max", cur) }
  else
    sub[1] = { position = app.GRID5_LINE2, justify = app.justifyCenter,
               text = "No buffer" }
  end
  return controls, menu, sub
end

-- ── views ───────────────────────────────────────────────────────────────
-- The Reel is the in-context graphic for every control; the waveform view
-- sits beside it in the expanded view (and is where the editor opens from).
local controlOrder = { "rec", "ext", "speed", "voct", "start", "len", "sos", "dry", "level" }
local views = { expanded = { "reel", "wave" }, collapsed = {} }
for _, name in ipairs(controlOrder) do
  views.expanded[#views.expanded + 1] = name
  views[name] = { "reel", name }
end

function Noether:onLoadViews(objects, branches)
  local controls = {}

  local ReelView = require "noether.ReelView"
  controls.reel = ReelView { name = "reel", head = objects.head, width = 2 * ply }
  local LoopView = require "noether.LoopView"
  controls.wave = LoopView { name = "wave", head = objects.head, width = 2 * ply }

  controls.rec = Gate { button = "rec", description = "rec / close / overdub",
                        branch = branches.rec, comparator = objects.rec }
  controls.ext = Gate { button = "ext", description = "extend: overdub past the end grows the loop (latched)",
                        branch = branches.ext, comparator = objects.extg }
  controls.voct = Pitch {
    button      = "V/oct",
    description = "1V/oct into speed",
    branch      = branches.voct,
    offset      = objects.tune,
    range       = objects.tuneRange,
  }

  local function gb(name, btn, desc, map, bias, gainMap)
    controls[name] = GainBias {
      button = btn, description = desc, branch = branches[name],
      gainbias = objects[name .. "Param"], range = objects[name .. "Range"],
      biasMap = map, initialBias = bias,
      gainMap = gainMap or Encoder.getMap("[-1,1]"),
    }
  end

  local speedMap = app.LinearDialMap(-4, 4); speedMap:setCoarseRadix(16)
  gb("speed", "speed", "Speed: -4x .. 0 (stop) .. +4x", speedMap, 1.0,
     app.LinearDialMap(-4, 4))
  gb("start", "start", "Window start (fraction of loop)", Encoder.getMap("[0,1]"), 0.0)
  gb("len",   "len",   "Window length (1 = whole loop)", Encoder.getMap("[0,1]"), 1.0)
  gb("sos",   "sos",   "SOS: 0 replace .. 1 keep loop", Encoder.getMap("[0,1]"), 0.5)
  gb("dry",   "dry",   "Live input level", Encoder.getMap("[0,1]"), 1.0)
  gb("level", "level", "Loop level", Encoder.getMap("[0,1]"), 1.0)

  return controls, views
end

return Noether
