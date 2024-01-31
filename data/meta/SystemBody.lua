-- Copyright © 2008-2024 Pioneer Developers. See AUTHORS.txt for details
-- Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

-- This file implements type information about C++ classes for Lua static analysis

---@meta

---@class SystemBody
---
---@field index integer
---@field name string
---@field type BodyType
---@field superType BodySuperType
---@field seed integer
---@field parent SystemBody?
---@field system StarSystem
---
--- The population of the body, in billions of people.
---@field population number
--- The radius of the body, in metres (m).
---@field radius number
--- The mass of the body, in kilograms (kg).
---@field mass number
--- The gravity on the surface of the body (m/s).
---@field gravity number
--- The periapsis of the body's orbit, in metres (m).
---@field periapsis number
--- The apoapsis of the body's orbit, in metres (m).
---@field apoapsis number
--- The orbit of the body, around its parent, in days
---@field orbitPeriod number
--- The rotation period of the body, in days
---@field rotationPeriod number
--- The semi-major axis of the orbit, in metres (m).
---@field semiMajorAxis number
--- The orbital eccentricity of the body
---@field eccentricity number
--- The axial tilt of the body, in radians
---@field axialTilt number
--- The average surface temperature of the body, in degrees kelvin
---@field averageTemp number
--- The measure of metallicity of the body's crust:
--- 0.0 = light (Al, SiO2, etc), 1.0 = heavy (Fe, heavy metals)
---@field metallicity number
--- The atmospheric density at "surface level" of the body:
--- 0.0 = no atmosphere, 1.225 = earth atmosphere density, 64 ~= venus
---@field atmosDensity number
--- The compositional value of any atmospheric gasses in the bodys atmosphere (if any):
--- 0.0 = reducing (H2, NH3, etc), 1.0 = oxidising (CO2, O2, etc)
---@field atmosOxidizing number
--- The pressure of the atmosphere at the body's mean "surface level":
--- 1.0atm = earth
---@field surfacePressure number
--- The measure of volatile liquids present on the body:
--- 0.0 = none, 1.0 = waterworld (earth = 70%)
---@field volatileLiquid number
--- The measure of volatile ices present on the body:
--- 0.0 = none, 1.0 = total ice cover (earth = 3%)
---@field volatileIces number
--- The measure of volcanicity of the body:
--- 0.0 = none, 1.0 = lava planet
---@field volcanicity number
--- The measure of life present on the body:
--- 0.0 = dead, 1.0 = teeming (~= pandora)
---@field life number
--- The measure of agricultural activity present on the body:
--- 0.0 = dead, 1.0 = teeming (~= breadbasket)
---@field agricultural number
---
---@field hasRings boolean
---@field hasAtmosphere boolean
---@field isScoopable boolean
---
---@field astroDescription string
---@field path SystemPath
---@field body Body?
---@field children SystemBody[]
---@field nearestJumpable SystemBody
---@field isMoon boolean
---@field isStation boolean
---@field isGroundStation boolean
---@field isSpaceStation boolean
---@field physicsBody Body?
