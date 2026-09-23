-- SPDX-License-Identifier: Apache-2.0
-- Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
--
-- ReelView.lua — display-only ViewControl hosting the Reel (the loop drawn as
-- a ring: envelope, window, sections, head with its comet tail) and, on the
-- sub display, the speed scale with the ratio / semitone / length / section
-- readouts. Subclasses the base ViewControl (nothing to zoom) — the same
-- shape as Dirac's GrainFieldView.

local app = app
local Class = require "Base.Class"
local ViewControl = require "Unit.ViewControl"
local libnoether = require "noether.libnoether"
local ply = app.SECTION_PLY

local ReelView = Class {}
ReelView:include(ViewControl)

function ReelView:init(args)
  ViewControl.init(self, args.name or "reel")
  self:setClassName("Noether.ReelView")
  local head = args.head or app.logError("%s.init: head is missing.", self)
  local width = args.width or (2 * ply)

  self.reel = libnoether.NoetherReel(0, 0, width, 64)
  self.reel:follow(head)

  local graphic = app.Graphic(0, 0, width, 64)
  graphic:addChild(self.reel)
  self:setControlGraphic(graphic)
  self:setMainCursorController(self.reel)

  for i = 1, (width // ply) do
    self:addSpotDescriptor{ center = (i - 0.5) * ply }
  end

  self.scale = libnoether.NoetherScale(0, 0, 128, 64)
  self.scale:follow(head)
  self.subGraphic = app.Graphic(0, 0, 128, 64)
  self.subGraphic:addChild(self.scale)
end

return ReelView
