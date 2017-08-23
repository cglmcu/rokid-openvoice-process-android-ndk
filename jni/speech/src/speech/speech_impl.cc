#include <chrono>
#include "speech_impl.h"

#define WS_SEND_TIMEOUT 10000

using std::shared_ptr;
using std::mutex;
using std::lock_guard;
using rokid::open::SpeechRequest;
using rokid::open::SpeechResponse;
using rokid::open::ReqType;
using rokid::open::SpeechErrorCode;

namespace rokid {
namespace speech {

SpeechImpl::SpeechImpl() : initialized_(false) {
}

bool SpeechImpl::prepare() {
	if (initialized_)
		return true;
	next_id_ = 0;
	connection_.initialize(SOCKET_BUF_SIZE, &config_, "speech");
	initialized_ = true;
	req_thread_ = new thread([=] { send_reqs(); });
	resp_thread_ = new thread([=] { gen_results(); });
	return true;
}

void SpeechImpl::release() {
	Log::d(tag__, "SpeechImpl.release, initialized = %d", initialized_);
	if (initialized_) {
		// notify req thread to exit
		unique_lock<mutex> req_locker(req_mutex_);
		initialized_ = false;
		connection_.release();
		voice_reqs_.close();
		text_reqs_.clear();
		req_cond_.notify_one();
		req_locker.unlock();
		req_thread_->join();
		delete req_thread_;

		// notify resp thread to exit
		unique_lock<mutex> resp_locker(resp_mutex_);
		responses_.close();
		controller_.finish_op();
		resp_cond_.notify_one();
		resp_locker.unlock();
		resp_thread_->join();
		delete resp_thread_;
	}
}

int32_t SpeechImpl::put_text(const char* text) {
	if (!initialized_ || text == NULL)
		return -1;
	int32_t id = next_id();
	lock_guard<mutex> locker(req_mutex_);
	shared_ptr<SpeechReqInfo> p(new SpeechReqInfo());
	p->id = id;
	p->type = SpeechReqType::TEXT;
	p->data.reset(new string(text));
	text_reqs_.push_back(p);
#ifdef SPEECH_SDK_DETAIL_TRACE
	Log::d(tag__, "put text %d, %s", id, text);
#endif
	req_cond_.notify_one();
	return id;
}

int32_t SpeechImpl::start_voice(shared_ptr<Options> framework_options,
		shared_ptr<Options> skill_options) {
	if (!initialized_)
		return -1;
	lock_guard<mutex> locker(req_mutex_);
	int32_t id = next_id();
	if (!voice_reqs_.start(id))
		return -1;
	shared_ptr<FSOptions> arg(new FSOptions());
	arg->framework_options = framework_options;
	arg->skill_options = skill_options;
	voice_reqs_.set_arg(id, arg);
#ifdef SPEECH_SDK_DETAIL_TRACE
	Log::d(tag__, "start voice %d", id);
#endif
	req_cond_.notify_one();
	return id;
}

void SpeechImpl::put_voice(int32_t id, const uint8_t* voice, uint32_t length) {
	if (!initialized_)
		return;
	if (id <= 0 || voice == NULL || length == 0)
		return;
	lock_guard<mutex> locker(req_mutex_);
	shared_ptr<string> spv(new string((const char*)voice, length));
	if (voice_reqs_.stream(id, spv)) {
#ifdef SPEECH_SDK_DETAIL_TRACE
		Log::d(tag__, "put voice %d, len %u", id, length);
#endif
		req_cond_.notify_one();
	}
}

void SpeechImpl::end_voice(int32_t id) {
	if (!initialized_)
		return;
	if (id <= 0)
		return;
	lock_guard<mutex> locker(req_mutex_);
	if (voice_reqs_.end(id)) {
#ifdef SPEECH_SDK_DETAIL_TRACE
		Log::d(tag__, "end voice %d", id);
#endif
		req_cond_.notify_one();
	}
}

void SpeechImpl::cancel(int32_t id) {
	lock_guard<mutex> req_locker(req_mutex_);
	if (!initialized_)
		return;
	Log::d(tag__, "cancel %d", id);
	if (id > 0) {
		if (voice_reqs_.erase(id)) {
			req_cond_.notify_one();
			return;
		}
		list<shared_ptr<SpeechReqInfo> >::iterator it;
		for (it = text_reqs_.begin(); it != text_reqs_.end(); ++it) {
			if ((*it)->id == id) {
				(*it)->type = SpeechReqType::CANCELLED;
				return;
			}
		}
		lock_guard<mutex> resp_locker(resp_mutex_);
		controller_.cancel_op(id, resp_cond_);
	} else {
		int32_t min_id;
		voice_reqs_.clear(&min_id, NULL);
		if (min_id > 0)
			req_cond_.notify_one();
		list<shared_ptr<SpeechReqInfo> >::iterator it;
		for (it = text_reqs_.begin(); it != text_reqs_.end(); ++it) {
			(*it)->type = SpeechReqType::CANCELLED;
		}
		lock_guard<mutex> resp_locker(resp_mutex_);
		controller_.cancel_op(0, resp_cond_);
	}
}

void SpeechImpl::config(const char* key, const char* value) {
	config_.set(key, value);
}

static SpeechResultType poptype_to_restype(int32_t type) {
	static SpeechResultType _tps[] = {
		SPEECH_RES_INTER,
		SPEECH_RES_START,
		SPEECH_RES_END,
		SPEECH_RES_CANCELLED,
		SPEECH_RES_ERROR,
	};
	assert(type >= 0 && type < sizeof(_tps)/sizeof(SpeechResultType));
	return _tps[type];
}

static SpeechError integer_to_reserr(uint32_t err) {
	switch (err) {
		case 0:
			return SPEECH_SUCCESS;
		case 2:
			return SPEECH_UNAUTHENTICATED;
		case 3:
			return SPEECH_CONNECTION_EXCEED;
		case 4:
			return SPEECH_SERVER_RESOURCE_EXHASTED;
		case 5:
			return SPEECH_SERVER_BUSY;
		case 6:
			return SPEECH_SERVER_INTERNAL;
		case 101:
			return SPEECH_SERVICE_UNAVAILABLE;
		case 102:
			return SPEECH_SDK_CLOSED;
	}
	return SPEECH_UNKNOWN;
}

bool SpeechImpl::poll(SpeechResult& res) {
	shared_ptr<SpeechOperationController::Operation> op;
	int32_t id;
	shared_ptr<SpeechResultIn> resin;
	int32_t poptype;
	uint32_t err = 0;

	res.err = SPEECH_SUCCESS;
	res.asr.clear();
	res.nlp.clear();
	res.action.clear();
	res.extra.clear();

	unique_lock<mutex> locker(resp_mutex_);
	while (initialized_) {
		op = controller_.front_op();
		if (op.get()) {
			if (op->status == SpeechStatus::CANCELLED) {
				if (responses_.erase(op->id)) {
					responses_.pop(id, resin, err);
					assert(id == op->id);
				}
				res.id = op->id;
				res.type = SPEECH_RES_CANCELLED;
				res.err = SPEECH_SUCCESS;
				controller_.remove_front_op();
				Log::d(tag__, "SpeechImpl.poll (%d) cancelled, "
						"remove front op", op->id);
				return true;
			} else if (op->status == SpeechStatus::ERROR) {
				if (responses_.erase(op->id)) {
					responses_.pop(id, resin, err);
					assert(id == op->id);
				}
				res.id = op->id;
				res.type = SPEECH_RES_ERROR;
				res.err = op->error;
				controller_.remove_front_op();
				Log::d(tag__, "SpeechImpl.poll (%d) error, "
						"remove front op", op->id);
				return true;
			} else {
				poptype = responses_.pop(id, resin, err);
				if (poptype != ReqStreamQueue::POP_TYPE_EMPTY) {
					assert(id == op->id);
					res.id = id;
					res.type = poptype_to_restype(poptype);
					res.err = integer_to_reserr(err);
					if (resin.get()) {
						res.asr = resin->asr;
						res.nlp = resin->nlp;
						res.action = resin->action;
						res.extra = resin->extra;
					}
					Log::d(tag__, "SpeechImpl.poll return result "
							"id(%d), type(%d)", res.id, res.type);
					if (res.type >= SPEECH_RES_END) {
						Log::d(tag__, "SpeechImpl.poll (%d) end", res.id);
						controller_.remove_front_op();
					}
					return true;
				}
			}
		}
		Log::d(tag__, "SpeechImpl.poll wait");
		resp_cond_.wait(locker);
	}
	Log::d(tag__, "SpeechImpl.poll return false, sdk released");
	return false;
}

static SpeechReqType sqtype_to_reqtype(int32_t type) {
	static SpeechReqType _tps[] = {
		SpeechReqType::VOICE_DATA,
		SpeechReqType::VOICE_START,
		SpeechReqType::VOICE_END,
		SpeechReqType::CANCELLED
	};
	assert(type >= 0 && type < sizeof(_tps)/sizeof(SpeechReqType));
	return _tps[type];
}

void SpeechImpl::send_reqs() {
	int32_t r;
	int32_t id;
	shared_ptr<string> voice;
	uint32_t err;
	int32_t rv;
	shared_ptr<SpeechReqInfo> info;
	bool opr;

	Log::d(tag__, "thread 'send_reqs' begin");
	while (true) {
		unique_lock<mutex> locker(req_mutex_);
		if (!initialized_)
			break;
		r = voice_reqs_.pop(id, voice, err);
		if (r >= 0) {
			info.reset(new SpeechReqInfo());
			info->id = id;
			info->type = sqtype_to_reqtype(r);
			info->data = voice;
			info->fsoptions = voice_reqs_.get_arg(id);
		} else if (!text_reqs_.empty()) {
			info = text_reqs_.front();
			text_reqs_.pop_front();
		} else {
			Log::d(tag__, "SpeechImpl.send_reqs wait req available");
			req_cond_.wait(locker);
			continue;
		}
		opr = do_ctl_change_op(info);
		locker.unlock();

		if (opr) {
			rv = do_request(info);
			if (rv == 0) {
				Log::d(tag__, "SpeechImpl.send_reqs wait op finish");
				unique_lock<mutex> resp_locker(resp_mutex_);
				controller_.wait_op_finish(info->id, resp_locker);
			}
		}
	}
	Log::d(tag__, "thread 'send_reqs' quit");
}

bool SpeechImpl::do_ctl_change_op(std::shared_ptr<SpeechReqInfo>& req) {
	shared_ptr<SpeechOperationController::Operation> op =
		controller_.current_op();
	if (req->type == SpeechReqType::TEXT
			|| req->type == SpeechReqType::VOICE_START) {
		Log::d(tag__, "do_ctl_change_op: req type is %d, new op START",
				req->type);
		assert(op.get() == NULL);
		controller_.new_op(req->id, SpeechStatus::START);
		return true;
	}
	if (op.get()) {
		if (req->type == SpeechReqType::VOICE_END
				|| req->type == SpeechReqType::VOICE_DATA)
			return true;
		assert(req->type == SpeechReqType::CANCELLED);
		op->status = SpeechStatus::CANCELLED;
		Log::d(tag__, "(%d) is processing, Status --> Cancelled",
				req->id);
		resp_cond_.notify_one();
		return true;
	}
	if (req->type == SpeechReqType::CANCELLED) {
		Log::d(tag__, "do_ctl_change_op: req type is %d, new op CANCELLED",
				req->type);
		controller_.new_op(req->id, SpeechStatus::CANCELLED);
		// no data send to server
		// notify 'poll' function to generate 'CANCEL' result
		resp_cond_.notify_one();
		return false;
	}
	return false;
}

static void req_config(SpeechRequest& req,
		shared_ptr<Options> framework_options,
		shared_ptr<Options> skill_options, SpeechConfig& config) {
	req.set_lang(config.get("lang", "zh"));
	req.set_codec(config.get("codec", "pcm"));
	req.set_vt(config.get("vt", ""));
	string json;
	if (framework_options.get()) {
		framework_options->to_json_string(json);
		req.set_framework_options(json);
#ifdef SPEECH_SDK_DETAIL_TRACE
		Log::d(tag__, "framework options is %s", json.c_str());
#endif
	}
	if (skill_options.get()) {
		skill_options->to_json_string(json);
		req.set_skill_options(json);
#ifdef SPEECH_SDK_DETAIL_TRACE
		Log::d(tag__, "skill options is %s", json.c_str());
#endif
	}
}

int32_t SpeechImpl::do_request(shared_ptr<SpeechReqInfo>& req) {
	SpeechRequest treq;
	int32_t rv = 1;
	switch (req->type) {
		case SpeechReqType::TEXT: {
			treq.set_id(req->id);
			treq.set_type(ReqType::TEXT);
			treq.set_asr(*req->data);
			req_config(treq, NULL, NULL, config_);
			rv = 0;

			Log::d(tag__, "SpeechImpl.do_request (%d) send text req",
					req->id);
			break;
		}
		case SpeechReqType::VOICE_START: {
			treq.set_id(req->id);
			treq.set_type(ReqType::START);
			req_config(treq, req->fsoptions->framework_options,
					req->fsoptions->skill_options, config_);

			Log::d(tag__, "SpeechImpl.do_request (%d) send voice start",
					req->id);
			break;
		}
		case SpeechReqType::VOICE_END:
			treq.set_id(req->id);
			treq.set_type(ReqType::END);
			rv = 0;
			Log::d(tag__, "SpeechImpl.do_request (%d) send voice end",
					req->id);
			break;
		case SpeechReqType::CANCELLED:
			treq.set_id(req->id);
			treq.set_type(ReqType::END);
			Log::d(tag__, "SpeechImpl.do_request (%d) send voice end"
					" because req cancelled", req->id);
			break;
		case SpeechReqType::VOICE_DATA:
			treq.set_id(req->id);
			treq.set_type(ReqType::VOICE);
			treq.set_voice(*req->data);
			Log::d(tag__, "SpeechImpl.do_request (%d) send voice data",
					req->id);
			break;
		default:
			Log::w(tag__, "SpeechImpl.do_request: (%d) req type is %u, "
					"it's impossible!", req->id, req->type);
			assert(false);
			return -1;
	}

	ConnectionOpResult r = connection_.send(treq, WS_SEND_TIMEOUT);
	if (r != ConnectionOpResult::SUCCESS) {
		SpeechError err = SPEECH_UNKNOWN;
		if (r == ConnectionOpResult::CONNECTION_NOT_AVAILABLE)
			err = SPEECH_SERVICE_UNAVAILABLE;
		Log::w(tag__, "SpeechImpl.do_request: (%d) send req failed "
				"%d, set op error", req->id, r);
		lock_guard<mutex> locker(resp_mutex_);
		controller_.set_op_error(err);
		resp_cond_.notify_one();
		return -1;
	} else if (rv == 0) {
#ifdef SPEECH_SDK_DETAIL_TRACE
		Log::d(tag__, "req (%d) last data sent, req done", req->id);
#endif
		lock_guard<mutex> locker(resp_mutex_);
		controller_.refresh_op_time();
	}
	return rv;
}

void SpeechImpl::gen_results() {
	SpeechResponse resp;
	ConnectionOpResult r;
	SpeechError err;
	uint32_t timeout;

	Log::d(tag__, "thread 'gen_results' run");
	while (true) {
		unique_lock<mutex> locker(resp_mutex_);
		timeout = controller_.op_timeout();
		locker.unlock();

#ifdef SPEECH_SDK_DETAIL_TRACE
		Log::d(tag__, "gen_results: recv with timeout %u", timeout);
#endif
		r = connection_.recv(resp, timeout);
		if (r == ConnectionOpResult::NOT_READY)
			break;
		locker.lock();
		if (r == ConnectionOpResult::SUCCESS) {
			gen_result_by_resp(resp);
		} else if (r == ConnectionOpResult::TIMEOUT) {
			if (controller_.op_timeout() == 0) {
				Log::w(tag__, "gen_results: (%d) op timeout, "
						"set op error", controller_.current_op()->id);
				controller_.set_op_error(SPEECH_TIMEOUT);
				resp_cond_.notify_one();
			}
		} else if (r == ConnectionOpResult::CONNECTION_BROKEN) {
			controller_.set_op_error(SPEECH_SERVICE_UNAVAILABLE);
			resp_cond_.notify_one();
		} else {
			controller_.set_op_error(SPEECH_UNKNOWN);
			resp_cond_.notify_one();
		}
		locker.unlock();
	}
	Log::d(tag__, "thread 'gen_results' quit");
}

void SpeechImpl::gen_result_by_resp(SpeechResponse& resp) {
	bool new_data = false;
	shared_ptr<SpeechOperationController::Operation> op =
		controller_.current_op();
	if (op.get() && op->id == resp.id()
			&& op->status != SpeechStatus::CANCELLED
			&& op->status != SpeechStatus::ERROR) {
		if (op->status == SpeechStatus::START) {
			responses_.start(resp.id());
			op->status = SpeechStatus::STREAMING;
			new_data = true;
			Log::d(tag__, "gen_result_by_resp(%d): push start resp, "
					"Status Start --> Streaming", resp.id());
		}

		if (resp.result() == SpeechErrorCode::SUCCESS) {
			Log::d(tag__, "SpeechResponse finish(%d)", resp.finish());
			shared_ptr<SpeechResultIn> resin(new SpeechResultIn());
			resin->asr = resp.asr();
			resin->nlp = resp.nlp();
			resin->action = resp.action();
			resin->extra = resp.extra();

			if (resp.finish()) {
				responses_.end(resp.id(), resin);
				new_data = true;
				op->status = SpeechStatus::END;
				Log::d(tag__, "gen_result_by_resp(%d): push end resp, "
						"Status Streaming --> End", resp.id());
				controller_.finish_op();
			} else {
				responses_.stream(resp.id(), resin);
				new_data = true;
				Log::d(tag__, "gen_result_by_resp(%d): push nlp resp "
						"%s", resp.id(), resin->action.c_str());
			}
		} else {
			responses_.erase(resp.id(), resp.result());
			new_data = true;
			controller_.finish_op();
		}

		if (new_data) {
			Log::d(tag__, "some responses put to queue, "
					"awake poll thread");
			resp_cond_.notify_one();
		}
	}
}

shared_ptr<Speech> new_speech() {
	return shared_ptr<Speech>(new SpeechImpl());
}

} // namespace speech
} // namespace rokid
