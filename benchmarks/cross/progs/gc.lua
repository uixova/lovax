local sink = 0
for i=0,299999 do
  local tmp = {i, i+1, i+2}
  sink = sink + tmp[1]
end
print(sink)
