local function make(d)
  if d == 0 then return {val=1, left=nil, right=nil} end
  return {val=1, left=make(d-1), right=make(d-1)}
end
local function check(n)
  if n.left == nil then return n.val end
  return n.val + check(n.left) + check(n.right)
end
local total = 0
for i=1,8 do total = total + check(make(15)) end
print(total)
