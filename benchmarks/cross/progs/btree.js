function make(d){ if(d===0) return {val:1,left:null,right:null};
  return {val:1,left:make(d-1),right:make(d-1)}; }
function check(n){ if(n.left===null) return n.val;
  return n.val + check(n.left) + check(n.right); }
let total = 0;
for (let i=0;i<8;i++) total += check(make(15));
console.log(total);
