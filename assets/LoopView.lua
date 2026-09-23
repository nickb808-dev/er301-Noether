-- SPDX-License-Identifier: Apache-2.0
-- Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
--
-- LoopView.lua — waveform + head position view for Noether.
-- core/Player/WaveForm.lua's shape with our NoetherDisplay (an od::HeadDisplay
-- subclass) in place of app.HeadDisplay: it follows the write head so the
-- waveform fills in as you record, marks the loop region, and prints the
-- state. app.HeadSubDisplay shows the position / length readout. Sub button 3
-- clears the loop (routed up to the unit, like WaveForm's "open editor").

local app = app
local Class = require "Base.Class"
local Zoomable = require "Unit.ViewControl.Zoomable"
local Channels = require "Channels"
local Signal = require "Signal"
local ply = app.SECTION_PLY
local libnoether = require "noether.libnoether"

local LoopView = Class {}
LoopView:include(Zoomable)

function LoopView:init(args)
  Zoomable.init(self)
  self:setClassName("Noether.LoopView")
  local width = args.width or (2 * ply)
  local head = args.head or app.logError("%s: head is missing from args.", self)
  self.head = head

  local graphic = app.Graphic(0, 0, width, 64)
  self.mainDisplay = libnoether.NoetherDisplay(head, 0, 0, width, 64)
  graphic:addChild(self.mainDisplay)
  self:setMainCursorController(self.mainDisplay)
  self:setControlGraphic(graphic)

  for i = 1, (width // ply) do
    self:addSpotDescriptor{ center = (i - 0.5) * ply }
  end
  self.verticalDivider = width

  self.subGraphic = app.Graphic(0, 0, 128, 64)
  self.subDisplay = app.HeadSubDisplay(head)
  self.subGraphic:addChild(self.subDisplay)

  self.subButton1 = app.SubButton("", 1)
  self.subGraphic:addChild(self.subButton1)
  self.subButton2 = app.SubButton("", 2)
  self.subGraphic:addChild(self.subButton2)
  self.subButton3 = app.SubButton("clear", 3)
  self.subGraphic:addChild(self.subButton3)

  Signal.weakRegister("selectReleased", self)
end

function LoopView:setSample(sample)
  if sample then
    self.subDisplay:setName(sample.name)
    self.mainDisplay:setChannel(Channels.getSide() - 1)
  end
end

function LoopView:selectReleased(i, shifted)
  self.mainDisplay:setChannel(Channels.getSide(i) - 1)
  return true
end

function LoopView:getFloatingMenuItems()
  local choices = Zoomable.getFloatingMenuItems(self)
  choices[#choices + 1] = "collapse"
  choices[#choices + 1] = "open editor"
  return choices
end

function LoopView:onFloatingMenuSelection(choice)
  if choice == "open editor" then
    self:callUp("showSampleEditor")
    return true
  elseif choice == "collapse" then
    self:callUp("switchView", "expanded")
  else
    return Zoomable.onFloatingMenuSelection(self, choice)
  end
end

function LoopView:subReleased(i, shifted)
  if shifted then
    return false
  elseif i == 3 then
    self:callUp("doClearLoop")
  end
  return true
end

return LoopView
