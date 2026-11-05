if o.cmd_present("omarchy-desktop-recorder") then
  o.bind("F10", "Start desktop audio recording", "omarchy-desktop-recorder press")
  o.bind("F10", "Stop desktop audio recording", "omarchy-desktop-recorder release", { release = true })
end
