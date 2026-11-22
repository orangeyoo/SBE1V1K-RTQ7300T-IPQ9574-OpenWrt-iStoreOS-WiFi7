-- istore_write_auth: istorex 写接口会话校验（r15，作者 orangeyoo）
-- 行为：要求调用方携带有效的 LuCI 登录会话（sysauth cookie）
local http = require "luci.http"
local util = require "luci.util"
local M = {}
function M.check()
	local sid = http.getcookie("sysauth")
	if not sid or sid == "" then return false end
	local ok, sess = pcall(util.ubus, "session", "list", { ubus_rpc_session = sid })
	if not ok then return false end
	return type(sess) == "table" and sess[sid] ~= nil
end
return M
