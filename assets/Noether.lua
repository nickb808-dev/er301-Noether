-- SPDX-License-Identifier: Apache-2.0
-- Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
--
-- Noether — seamless vari-speed loop recorder.
-- One REC button (empty -> record -> play -> overdub in -> overdub out), a
-- phase-matched seam, continuous Speed from -2x to +2x plus 1 V/oct, Extend
-- (overdub past the end grows the loop), undo, stop, a clock input, SOS
-- crossfader, Dry / Level, and loops that are saved to the card with the
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
local OptionControl = require "Unit.MenuControl.OptionControl"
local Overlay = require "Overlay"
local ply = app.SECTION_PLY

local libnoether = require "noether.libnoether"

-- THE RUNNING VERSION, ON SCREEN (standard §10c). tools/check-version.sh
-- fails the build if this drifts from the Makefile or toc.lua.
local VERSION = "0.7.2"

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

  -- UNDO: a trigger. Swaps the last overdub / extend pass out; again = redo.
  local undo = self:addObject("undo", app.Comparator())
  connect(undo, "Out", head, "Undo")
  self:addMonoBranch("undo", undo, "In", undo, "Out")

  -- CLK: a trigger. Free mode: restarts the loop. Sync mode: the clock that
  -- quantises REC and re-syncs the loop every N edges.
  local clk = self:addObject("clk", app.Comparator())
  connect(clk, "Out", head, "Clk")
  self:addMonoBranch("clk", clk, "In", clk, "Out")

  -- STOP: a latched gate, off by default = playing. (A "play" gate that
  -- starts ON would need a simulated edge at load, which races a preset's
  -- restore of the Comparator state — so the polarity is the safe one.)
  local stop = self:addObject("stop", app.Comparator())
  stop:setToggleMode()
  connect(stop, "Out", head, "Stop")
  self:addMonoBranch("stop", stop, "In", stop, "Out")

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

-- The undo buffer: a second pool buffer of the same size, held by the unit
-- alone (it is never attached to the head's Sample, so the waveform view and
-- the editor never see it). Same claim / release / unload discipline.
function Noether:setUndoBuffer(sample)
  local old = self.undoSample
  if old then
    old:release(self)
    self.undoSample = nil
  end
  self.undoSample = sample
  if sample then
    sample:claim(self)
    self.objects.head:setUndoSample(sample.pSample)
  else
    self.objects.head:setUndoSample(nil)
  end
  if old and old ~= sample and not old:isShared() then
    SamplePool.unload(old)
  end
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
    return
  end
  local usample = SamplePool.create {
    root = "noether-undo",
    channels = self.channelCount,
    secs = secs
  }
  if usample then
    self:setUndoBuffer(usample)
  else
    self:setUndoBuffer(nil)
    Overlay.flashMainMessage("No memory for undo (%d s)", secs)
  end
end

function Noether:doSetBufferSecs(secs)
  self:createBuffer(secs)
  Overlay.flashMainMessage("Loop buffer: %d s", secs)
end

function Noether:doClearLoop()
  self.objects.head:clearLoop()
  self.objects.head:zeroBuffer()
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
    self:setPersistStatus("saving %s", Path.getFilename(path))
  else
    self:setPersistStatus("save refused (card?)")
    SamplePool.unload(export)
  end
end

-- Every step of the load / save path records a one-line status, shown on
-- the menu's sub display (standard §15: put the state that explains a
-- failure somewhere reachable without the thing that fails) and logged as
-- a breadcrumb for crash reports.
function Noether:setPersistStatus(fmt, ...)
  local ok, msg = pcall(string.format, fmt, ...)
  self.persistStatus = ok and msg or fmt
  app.logInfo("Noether: %s", self.persistStatus)
end

function Noether:sampleStatusChanged(sample)
  if sample == nil then return end
  if sample == self.savingSample and not sample:isPending() then
    self.savingSample = nil
    self:setPersistStatus("saved %s", Path.getFilename(sample.path or "?"))
    if sample.userCount == 0 then SamplePool.unload(sample) end
  elseif sample == self.pendingLoop and not sample:isPending() then
    self.pendingLoop = nil
    local ok, n = pcall(function() return self.objects.head:importLoop(sample.pSample) end)
    if ok and n and n > 0 then
      self.loopRevSaved = self.objects.head:getLoopRev()
      self:setPersistStatus("loaded %d frames, state %d", n, self.objects.head:getState())
    else
      -- say what the engine saw, not just that it said no
      local ch = sample.getChannelCount and sample:getChannelCount() or -1
      local len = sample.length and sample:length() or -1
      local st = sample.getStatusText and sample:getStatusText() or "?"
      self:setPersistStatus("refused %s: ch %s len %s st %s %s", tostring(n), tostring(ch), tostring(len), tostring(st), tostring(sample.reason or ""))
      app.logError("%s: loop file did not import (%s)", self, self.persistStatus)
    end
    if sample.userCount == 0 then SamplePool.unload(sample) end
  elseif sample == self.pendingLoop then
    self:setPersistStatus("loading %s", Path.getFilename(sample.path or "?"))
  end
end

-- Load the loop file at self.loopPath into the buffer (deserialize, and the
-- menu's Reload task).
function Noether:loadLoopFromCard()
  local path = self.loopPath
  if not path then self:setPersistStatus("no loop file"); return end
  if not Path.exists(path) then self:setPersistStatus("missing: %s", Path.getFilename(path)); return end
  local s, status = SamplePool.load(path)
  if not s then self:setPersistStatus("load failed: %s", tostring(status)); return end
  self.pendingLoop = s
  self:setPersistStatus("load queued (%s)", s:isPending() and "pending" or "ready")
  if not s:isPending() then self:sampleStatusChanged(s) end
end

function Noether:serialize()
  local t = Unit.serialize(self)
  if self.sample then
    t.sample = SamplePool.serializeSample(self.sample)
  end
  t.bufferSecs = self.bufferSecs
  local head = self.objects.head
  t.syncN = head:getSyncN()
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
  if t.syncN then self.objects.head:setSyncN(t.syncN) end
  if t.sample then
    local sample = SamplePool.deserializeSample(t.sample, self.chain)
    if sample then
      self:setSample(sample)
      -- the undo buffer follows the loop buffer's size
      local usample = SamplePool.create {
        root = "noether-undo",
        channels = self.channelCount,
        secs = self.bufferSecs or kDefaultSecs
      }
      self:setUndoBuffer(usample or nil)
    else
      app.logError("%s:deserialize: failed to load sample.", self)
    end
  end
  if t.loopPath then
    self.loopPath = t.loopPath
    self:loadLoopFromCard()
  else
    self:setPersistStatus("preset carries no loop file")
  end
end

function Noether:onRemove()
  self:setUndoBuffer(nil)
  self:setSample(nil)
  Unit.onRemove(self)
end

-- ── menu ────────────────────────────────────────────────────────────────
-- Tasks, not OptionControls: creating a buffer allocates, which the audio
-- thread may never do (standard §2 / §3).
local menu = {
  "clockHeader", "sync",
  "bufferHeader", "buf10", "buf30", "buf60",
  "loopHeader", "clearLoop", "attachExisting", "editBuffer", "reloadLoop",
}

function Noether:onShowMenu(objects, branches)
  local controls = {}
  controls.clockHeader = MenuHeader { description = "Clock  (Noether v" .. VERSION .. ")" }
  -- free: clk restarts the loop. sync: rec waits for the next clock edge,
  -- the loop is a whole number of clock periods, and every N-th edge pulls
  -- the head back to the seam. Two choices (the OptionControl limit is 3).
  controls.sync = OptionControl {
    description = "Clk input",
    option      = objects.head:getOption("Sync"),
    choices     = { "free", "sync" },
    descriptionWidth = 2,
  }
  controls.bufferHeader = MenuHeader { description = "Loop buffer" }
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
  controls.reloadLoop = Task { description = "Reload loop file",
    task = function() self:loadLoopFromCard() end }

  local sub = {}
  if self.sample then
    sub[1] = { position = app.GRID5_LINE1, justify = app.justifyLeft, text = "Buffer:" }
    sub[2] = { position = app.GRID5_LINE2, justify = app.justifyLeft,
               text = self.sample.name or "attached" }
    sub[3] = { position = app.GRID5_LINE3, justify = app.justifyLeft,
               text = string.format("%d s max", cur) }
    sub[4] = { position = app.GRID5_LINE4, justify = app.justifyLeft,
               text = "file: " .. (self.persistStatus or "none yet") }
  else
    sub[1] = { position = app.GRID5_LINE2, justify = app.justifyCenter,
               text = "No buffer" }
  end
  return controls, menu, sub
end

-- ── views ───────────────────────────────────────────────────────────────
-- The Reel is the in-context graphic for every control; the waveform view
-- sits beside it in the expanded view (and is where the editor opens from).
local controlOrder = { "rec", "undo", "stop", "clk", "ext", "speed", "voct", "sos", "dry", "level" }
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
  controls.undo = Gate { button = "undo", description = "undo last pass (again = redo)",
                         branch = branches.undo, comparator = objects.undo }
  controls.stop = Gate { button = "stop", description = "stop (latched): fades out, holds the head",
                         branch = branches.stop, comparator = objects.stop }
  controls.clk = Gate { button = "clk", description = "free: restart loop / sync: the clock",
                        branch = branches.clk, comparator = objects.clk }
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

  -- -2..+2 with a coarse radix of 16: each coarse step is exactly 0.25, so
  -- the coarse dial lands on 1/4, 1/2, 1, 2 (and 0) by itself
  local speedMap = app.LinearDialMap(-2, 2); speedMap:setCoarseRadix(16)
  gb("speed", "speed", "Speed: -2x .. 0 (stop) .. +2x", speedMap, 1.0,
     app.LinearDialMap(-2, 2))
  gb("sos",   "sos",   "SOS: 0 replace .. 1 keep loop", Encoder.getMap("[0,1]"), 0.5)
  gb("dry",   "dry",   "Live input level", Encoder.getMap("[0,1]"), 1.0)
  gb("level", "level", "Loop level", Encoder.getMap("[0,1]"), 1.0)

  return controls, views
end

return Noether
