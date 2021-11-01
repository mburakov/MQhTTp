switch_state = false

publish('cmnd/torsh/Color', '#00000000')
subscribe('zigbee2mqtt/switch', function(payload)
  print('Switch says: ' .. payload)
  if string.find(payload, '"action":"on"') ~= nil and switch_state == false then
    switch_state = true
    publish('cmnd/torsh/Color', '#000000ff')
  elseif string.find(payload, '"action":"off"') ~= nil and switch_state == true then
    switch_state = false
    publish('cmnd/torsh/Color', '#00000000')
  end
end)
