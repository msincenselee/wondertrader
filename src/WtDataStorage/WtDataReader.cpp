#include "WtDataReader.h"

#include "../Includes/WTSVariant.hpp"
#include "../Share/ModuleHelper.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSDataDef.hpp"

#include "../WTSTools/WTSCmpHelper.hpp"

#include <rapidjson/document.h>
namespace rj = rapidjson;

extern "C"
{
	EXPORT_FLAG IDataReader* createDataReader()
	{
		IDataReader* ret = new WtDataReader();
		return ret;
	}

	EXPORT_FLAG void deleteDataReader(IDataReader* reader)
	{
		if (reader != NULL)
			delete reader;
	}
};


WtDataReader::WtDataReader()
	: _last_time(0)
	, _base_data_mgr(NULL)
	, _hot_mgr(NULL)
{
}


WtDataReader::~WtDataReader()
{
}

void WtDataReader::init(WTSVariant* cfg, IDataReaderSink* sink, IHisDataLoader* loader /* = NULL */)
{
	IDataReader::init(cfg, sink, loader);

	_base_data_mgr = sink->get_basedata_mgr();
	_hot_mgr = sink->get_hot_mgr();

	if (cfg == NULL)
		return ;

	_base_dir = cfg->getCString("path");
	_base_dir = StrUtil::standardisePath(_base_dir);
	
	/*
	 *	By Wesley @ 2021.12.20
	 *	�ȴ�extloader���س�Ȩ����
	 *	�������ʧ�ܣ����������˳�Ȩ�����ļ����ټ��س�Ȩ�����ļ�
	 */
	bool bLoaded = loadStkAdjFactorsFromLoader();

	if (!bLoaded && cfg->has("adjfactor"))
		loadStkAdjFactorsFromFile(cfg->getCString("adjfactor"));
}

bool WtDataReader::loadStkAdjFactorsFromLoader()
{
	if (NULL == _loader)
		return false;

	bool ret = _loader->loadAllAdjFactors(&_adj_factors, [](void* obj, const char* stdCode, uint32_t* dates, double* factors, uint32_t count) {
		AdjFactorMap* fact_map = (AdjFactorMap*)obj;
		AdjFactorList& fctrLst = (*fact_map)[stdCode];

		for(uint32_t i = 0; i < count; i++)
		{
			AdjFactor adjFact;
			adjFact._date = dates[i];
			adjFact._factor = factors[i];

			fctrLst.emplace_back(adjFact);
		}

		//һ��Ҫ�ѵ�һ���ӽ�ȥ����Ȼ�����ǰ��Ȩ�Ļ������ܻ�©�������������
		AdjFactor adjFact;
		adjFact._date = 19900101;
		adjFact._factor = 1;
		fctrLst.emplace_back(adjFact);

		std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right) {
			return left._date < right._date;
		});
	});

	if (ret && _sink) _sink->reader_log(LL_INFO, "Adjusting factors of %u contracts loaded via extended loader", _adj_factors.size());
	return ret;
}

bool WtDataReader::loadStkAdjFactorsFromFile(const char* adjfile)
{
	if(!StdFile::exists(adjfile))
	{
		if (_sink) _sink->reader_log(LL_ERROR, "��Ȩ�����ļ�%s������", adjfile);
		return false;
	}

	std::string content;
    StdFile::read_file_content(adjfile, content);

	rj::Document doc;
	doc.Parse(content.c_str());

	if(doc.HasParseError())
	{
		if (_sink) _sink->reader_log(LL_ERROR, "��Ȩ�����ļ�%s����ʧ��", adjfile);
		return false;
	}

	uint32_t stk_cnt = 0;
	uint32_t fct_cnt = 0;
	for (auto& mExchg : doc.GetObject())
	{
		const char* exchg = mExchg.name.GetString();
		const rj::Value& itemExchg = mExchg.value;
		for(auto& mCode : itemExchg.GetObject())
		{
			std::string code = mCode.name.GetString();
			const rj::Value& ayFacts = mCode.value;
			if(!ayFacts.IsArray() )
				continue;

			/*
			 *	By Wesley @ 2021.12.21
			 *	�ȼ��code�ĸ�ʽ�ǲ��ǰ���PID����STK.600000
			 *	�������PID����ֱ�Ӹ�ʽ�����������������ǿ��ΪSTK
			 */
			bool bHasPID = (code.find('.') != std::string::npos);

			std::string key;
			if (bHasPID)
				key = StrUtil::printf("%s.%s", exchg, code);
			else
				key = StrUtil::printf("%s.STK.s", exchg, code);

			stk_cnt++;

			AdjFactorList& fctrLst = _adj_factors[key];
			for (auto& fItem : ayFacts.GetArray())
			{
				AdjFactor adjFact;
				adjFact._date = fItem["date"].GetUint();
				adjFact._factor = fItem["factor"].GetDouble();

				fctrLst.emplace_back(adjFact);
				fct_cnt++;
			}

			//һ��Ҫ�ѵ�һ���ӽ�ȥ����Ȼ�����ǰ��Ȩ�Ļ������ܻ�©�������������
			AdjFactor adjFact;
			adjFact._date = 19900101;
			adjFact._factor = 1;
			fctrLst.emplace_back(adjFact);

			std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right) {
				return left._date < right._date;
			});
		}
	}

	if (_sink) _sink->reader_log(LL_INFO, "������%uֻ��Ʊ��%u����Ȩ��������", stk_cnt, fct_cnt);
	return true;
}

WTSTickSlice* WtDataReader::readTickSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	std::string stdPID = StrUtil::printf("%s.%s", cInfo._exchg, cInfo._product);

	uint32_t curDate, curTime, curSecs;
	if (etime == 0)
	{
		curDate = _sink->get_date();
		curTime = _sink->get_min_time();
		curSecs = _sink->get_secs();

		etime = (uint64_t)curDate * 1000000000 + curTime * 100000 + curSecs;
	}
	else
	{
		//20190807124533900
		curDate = (uint32_t)(etime / 1000000000);
		curTime = (uint32_t)(etime % 1000000000) / 100000;
		curSecs = (uint32_t)(etime % 100000);
	}

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), curDate, curTime, false);
	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), 0, 0, false);

	bool isToday = (endTDate == curTDate);

	std::string curCode = cInfo._code;
	if (cInfo.isHot() && cInfo.isFuture())
		curCode = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, endTDate);
	else if (cInfo.isSecond() && cInfo.isFuture())
		curCode = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, endTDate);

	std::vector<WTSTickStruct>	ayTicks;

	//�Ƚ�ʱ��Ķ���
	WTSTickStruct eTick;
	eTick.action_date = curDate;
	eTick.action_time = curTime * 100000 + curSecs;

	if (isToday)
	{
		TickBlockPair* tPair = getRTTickBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL)
			return NULL;

		RTTickBlock* tBlock = tPair->_block;

		WTSTickStruct* pTick = std::lower_bound(tBlock->_ticks, tBlock->_ticks + (tBlock->_size - 1), eTick, [](const WTSTickStruct& a, const WTSTickStruct& b){
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pTick - tBlock->_ticks;

		//�����궨λ��tickʱ���Ŀ��ʱ���, ��ȫ������һ��
		if (pTick->action_date > eTick.action_date || pTick->action_time>eTick.action_time)
		{
			pTick--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSTickSlice* slice = WTSTickSlice::create(stdCode, tBlock->_ticks + sIdx, cnt);
		return slice;
	}
	else
	{
		std::string key = StrUtil::printf("%s-%d", stdCode, endTDate);

		auto it = _his_tick_map.find(key);
		if(it == _his_tick_map.end())
		{
			std::stringstream ss;
			ss << _base_dir << "his/ticks/" << cInfo._exchg << "/" << endTDate << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				return NULL;

			HisTBlockPair& tBlkPair = _his_tick_map[key];
			StdFile::read_file_content(filename.c_str(), tBlkPair._buffer);
			if (tBlkPair._buffer.size() < sizeof(HisTickBlock))
			{
				if (_sink) _sink->reader_log(LL_ERROR, "��ʷTick�����ļ�%s��СУ��ʧ��", filename.c_str());
				tBlkPair._buffer.clear();
				return NULL;
			}

			HisTickBlock* tBlock = (HisTickBlock*)tBlkPair._buffer.c_str();
			if (tBlock->_version == BLOCK_VERSION_CMP)
			{
				//ѹ���汾,Ҫ���¼���ļ���С
				HisTickBlockV2* tBlockV2 = (HisTickBlockV2*)tBlkPair._buffer.c_str();

				if (tBlkPair._buffer.size() != (sizeof(HisTickBlockV2) + tBlockV2->_size))
				{
					if (_sink) _sink->reader_log(LL_ERROR, "��ʷTick�����ļ�%s��СУ��ʧ��", filename.c_str());
					return NULL;
				}

				//��Ҫ��ѹ
				std::string buf = WTSCmpHelper::uncompress_data(tBlockV2->_data, (uint32_t)tBlockV2->_size);

				//��ԭ����bufferֻ����һ��ͷ��,��������tick����׷�ӵ�β��
				tBlkPair._buffer.resize(sizeof(HisTickBlock));
				tBlkPair._buffer.append(buf);
				tBlockV2->_version = BLOCK_VERSION_RAW;
			}
			
			tBlkPair._block = (HisTickBlock*)tBlkPair._buffer.c_str();
		}
		
		HisTBlockPair& tBlkPair = _his_tick_map[key];
		if (tBlkPair._block == NULL)
			return NULL;

		HisTickBlock* tBlock = tBlkPair._block;

		uint32_t tcnt = (tBlkPair._buffer.size() - sizeof(HisTickBlock)) / sizeof(WTSTickStruct);
		if (tcnt <= 0)
			return NULL;

		WTSTickStruct* pTick = std::lower_bound(tBlock->_ticks, tBlock->_ticks + (tcnt - 1), eTick, [](const WTSTickStruct& a, const WTSTickStruct& b){
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pTick - tBlock->_ticks;
		if (pTick->action_date > eTick.action_date || pTick->action_time >= eTick.action_time)
		{
			pTick--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSTickSlice* slice = WTSTickSlice::create(stdCode, tBlock->_ticks + sIdx, cnt);
		return slice;
	}
}

WTSOrdQueSlice* WtDataReader::readOrdQueSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	std::string stdPID = StrUtil::printf("%s.%s", cInfo._exchg, cInfo._product);

	uint32_t curDate, curTime, curSecs;
	if (etime == 0)
	{
		curDate = _sink->get_date();
		curTime = _sink->get_min_time();
		curSecs = _sink->get_secs();

		etime = (uint64_t)curDate * 1000000000 + curTime * 100000 + curSecs;
	}
	else
	{
		//20190807124533900
		curDate = (uint32_t)(etime / 1000000000);
		curTime = (uint32_t)(etime % 1000000000) / 100000;
		curSecs = (uint32_t)(etime % 100000);
	}

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), curDate, curTime, false);
	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), 0, 0, false);

	bool isToday = (endTDate == curTDate);

	std::string curCode = cInfo._code;
	if (cInfo.isHot() && cInfo.isFuture())
		curCode = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, endTDate);
	else if (cInfo.isSecond() && cInfo.isFuture())
		curCode = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, endTDate);

	//�Ƚ�ʱ��Ķ���
	WTSOrdQueStruct eTick;
	eTick.action_date = curDate;
	eTick.action_time = curTime * 100000 + curSecs;

	if (isToday)
	{
		OrdQueBlockPair* tPair = getRTOrdQueBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL)
			return NULL;

		RTOrdQueBlock* rtBlock = tPair->_block;

		WTSOrdQueStruct* pItem = std::lower_bound(rtBlock->_queues, rtBlock->_queues + (rtBlock->_size - 1), eTick, [](const WTSOrdQueStruct& a, const WTSOrdQueStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pItem - rtBlock->_queues;

		//�����궨λ��tickʱ���Ŀ��ʱ���, ��ȫ������һ��
		if (pItem->action_date > eTick.action_date || pItem->action_time > eTick.action_time)
		{
			pItem--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSOrdQueSlice* slice = WTSOrdQueSlice::create(stdCode, rtBlock->_queues + sIdx, cnt);
		return slice;
	}
	else
	{
		std::string key = StrUtil::printf("%s-%d", stdCode, endTDate);

		auto it = _his_ordque_map.find(key);
		if (it == _his_ordque_map.end())
		{
			std::stringstream ss;
			ss << _base_dir << "his/queue/" << cInfo._exchg << "/" << endTDate << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				return NULL;

			HisOrdQueBlockPair& hisBlkPair = _his_ordque_map[key];
			StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer);
			if (hisBlkPair._buffer.size() < sizeof(HisOrdQueBlockV2))
			{
				if (_sink) _sink->reader_log(LL_ERROR, "��ʷί�ж��������ļ�%s��СУ��ʧ��", filename.c_str());
				hisBlkPair._buffer.clear();
				return NULL;
			}

			HisOrdQueBlockV2* tBlockV2 = (HisOrdQueBlockV2*)hisBlkPair._buffer.c_str();

			if (hisBlkPair._buffer.size() != (sizeof(HisOrdQueBlockV2) + tBlockV2->_size))
			{
				if (_sink) _sink->reader_log(LL_ERROR, "��ʷί�ж��������ļ�%s��СУ��ʧ��", filename.c_str());
				return NULL;
			}

			//��Ҫ��ѹ
			std::string buf = WTSCmpHelper::uncompress_data(tBlockV2->_data, (uint32_t)tBlockV2->_size);

			//��ԭ����bufferֻ����һ��ͷ��,��������tick����׷�ӵ�β��
			hisBlkPair._buffer.resize(sizeof(HisOrdQueBlock));
			hisBlkPair._buffer.append(buf);
			tBlockV2->_version = BLOCK_VERSION_RAW;

			hisBlkPair._block = (HisOrdQueBlock*)hisBlkPair._buffer.c_str();
		}

		HisOrdQueBlockPair& tBlkPair = _his_ordque_map[key];
		if (tBlkPair._block == NULL)
			return NULL;

		HisOrdQueBlock* tBlock = tBlkPair._block;

		uint32_t tcnt = (tBlkPair._buffer.size() - sizeof(HisOrdQueBlock)) / sizeof(WTSOrdQueStruct);
		if (tcnt <= 0)
			return NULL;

		WTSOrdQueStruct* pTick = std::lower_bound(tBlock->_items, tBlock->_items + (tcnt - 1), eTick, [](const WTSOrdQueStruct& a, const WTSOrdQueStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pTick - tBlock->_items;
		if (pTick->action_date > eTick.action_date || pTick->action_time >= eTick.action_time)
		{
			pTick--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSOrdQueSlice* slice = WTSOrdQueSlice::create(stdCode, tBlock->_items + sIdx, cnt);
		return slice;
	}
}

WTSOrdDtlSlice* WtDataReader::readOrdDtlSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	std::string stdPID = StrUtil::printf("%s.%s", cInfo._exchg, cInfo._product);

	uint32_t curDate, curTime, curSecs;
	if (etime == 0)
	{
		curDate = _sink->get_date();
		curTime = _sink->get_min_time();
		curSecs = _sink->get_secs();

		etime = (uint64_t)curDate * 1000000000 + curTime * 100000 + curSecs;
	}
	else
	{
		//20190807124533900
		curDate = (uint32_t)(etime / 1000000000);
		curTime = (uint32_t)(etime % 1000000000) / 100000;
		curSecs = (uint32_t)(etime % 100000);
	}

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), curDate, curTime, false);
	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), 0, 0, false);

	bool isToday = (endTDate == curTDate);

	std::string curCode = cInfo._code;
	if (cInfo.isHot() && cInfo.isFuture())
		curCode = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, endTDate);
	else if (cInfo.isSecond() && cInfo.isFuture())
		curCode = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, endTDate);

	//�Ƚ�ʱ��Ķ���
	WTSOrdDtlStruct eTick;
	eTick.action_date = curDate;
	eTick.action_time = curTime * 100000 + curSecs;

	if (isToday)
	{
		OrdDtlBlockPair* tPair = getRTOrdDtlBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL)
			return NULL;

		RTOrdDtlBlock* rtBlock = tPair->_block;

		WTSOrdDtlStruct* pItem = std::lower_bound(rtBlock->_details, rtBlock->_details + (rtBlock->_size - 1), eTick, [](const WTSOrdDtlStruct& a, const WTSOrdDtlStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pItem - rtBlock->_details;

		//�����궨λ��tickʱ���Ŀ��ʱ���, ��ȫ������һ��
		if (pItem->action_date > eTick.action_date || pItem->action_time > eTick.action_time)
		{
			pItem--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create(stdCode, rtBlock->_details + sIdx, cnt);
		return slice;
	}
	else
	{
		std::string key = StrUtil::printf("%s-%d", stdCode, endTDate);

		auto it = _his_ordque_map.find(key);
		if (it == _his_ordque_map.end())
		{
			std::stringstream ss;
			ss << _base_dir << "his/orders/" << cInfo._exchg << "/" << endTDate << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				return NULL;

			HisOrdDtlBlockPair& hisBlkPair = _his_orddtl_map[key];
			StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer);
			if (hisBlkPair._buffer.size() < sizeof(HisOrdDtlBlockV2))
			{
				if (_sink) _sink->reader_log(LL_ERROR, "��ʷ���ί�������ļ�%s��СУ��ʧ��", filename.c_str());
				hisBlkPair._buffer.clear();
				return NULL;
			}

			HisOrdDtlBlockV2* tBlockV2 = (HisOrdDtlBlockV2*)hisBlkPair._buffer.c_str();

			if (hisBlkPair._buffer.size() != (sizeof(HisOrdDtlBlockV2) + tBlockV2->_size))
			{
				if (_sink) _sink->reader_log(LL_ERROR, "��ʷ���ί�������ļ�%s��СУ��ʧ��", filename.c_str());
				return NULL;
			}

			//��Ҫ��ѹ
			std::string buf = WTSCmpHelper::uncompress_data(tBlockV2->_data, (uint32_t)tBlockV2->_size);

			//��ԭ����bufferֻ����һ��ͷ��,��������tick����׷�ӵ�β��
			hisBlkPair._buffer.resize(sizeof(HisOrdDtlBlock));
			hisBlkPair._buffer.append(buf);
			tBlockV2->_version = BLOCK_VERSION_RAW;

			hisBlkPair._block = (HisOrdDtlBlock*)hisBlkPair._buffer.c_str();
		}

		HisOrdDtlBlockPair& tBlkPair = _his_orddtl_map[key];
		if (tBlkPair._block == NULL)
			return NULL;

		HisOrdDtlBlock* tBlock = tBlkPair._block;

		uint32_t tcnt = (tBlkPair._buffer.size() - sizeof(HisOrdDtlBlock)) / sizeof(WTSOrdDtlStruct);
		if (tcnt <= 0)
			return NULL;

		WTSOrdDtlStruct* pTick = std::lower_bound(tBlock->_items, tBlock->_items + (tcnt - 1), eTick, [](const WTSOrdDtlStruct& a, const WTSOrdDtlStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pTick - tBlock->_items;
		if (pTick->action_date > eTick.action_date || pTick->action_time >= eTick.action_time)
		{
			pTick--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create(stdCode, tBlock->_items + sIdx, cnt);
		return slice;
	}
}

WTSTransSlice* WtDataReader::readTransSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	std::string stdPID = StrUtil::printf("%s.%s", cInfo._exchg, cInfo._product);

	uint32_t curDate, curTime, curSecs;
	if (etime == 0)
	{
		curDate = _sink->get_date();
		curTime = _sink->get_min_time();
		curSecs = _sink->get_secs();

		etime = (uint64_t)curDate * 1000000000 + curTime * 100000 + curSecs;
	}
	else
	{
		//20190807124533900
		curDate = (uint32_t)(etime / 1000000000);
		curTime = (uint32_t)(etime % 1000000000) / 100000;
		curSecs = (uint32_t)(etime % 100000);
	}

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), curDate, curTime, false);
	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), 0, 0, false);

	bool isToday = (endTDate == curTDate);

	std::string curCode = cInfo._code;
	if (cInfo.isHot() && cInfo.isFuture())
		curCode = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, endTDate);
	else if (cInfo.isSecond() && cInfo.isFuture())
		curCode = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, endTDate);

	//�Ƚ�ʱ��Ķ���
	WTSTransStruct eTick;
	eTick.action_date = curDate;
	eTick.action_time = curTime * 100000 + curSecs;

	if (isToday)
	{
		TransBlockPair* tPair = getRTTransBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL)
			return NULL;

		RTTransBlock* rtBlock = tPair->_block;

		WTSTransStruct* pItem = std::lower_bound(rtBlock->_trans, rtBlock->_trans + (rtBlock->_size - 1), eTick, [](const WTSTransStruct& a, const WTSTransStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pItem - rtBlock->_trans;

		//�����궨λ��tickʱ���Ŀ��ʱ���, ��ȫ������һ��
		if (pItem->action_date > eTick.action_date || pItem->action_time > eTick.action_time)
		{
			pItem--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSTransSlice* slice = WTSTransSlice::create(stdCode, rtBlock->_trans + sIdx, cnt);
		return slice;
	}
	else
	{
		std::string key = StrUtil::printf("%s-%d", stdCode, endTDate);

		auto it = _his_ordque_map.find(key);
		if (it == _his_ordque_map.end())
		{
			std::stringstream ss;
			ss << _base_dir << "his/trans/" << cInfo._exchg << "/" << endTDate << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				return NULL;

			HisTransBlockPair& hisBlkPair = _his_trans_map[key];
			StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer);
			if (hisBlkPair._buffer.size() < sizeof(HisTransBlockV2))
			{
				if (_sink) _sink->reader_log(LL_ERROR, "��ʷ��ʳɽ������ļ�%s��СУ��ʧ��", filename.c_str());
				hisBlkPair._buffer.clear();
				return NULL;
			}

			HisTransBlockV2* tBlockV2 = (HisTransBlockV2*)hisBlkPair._buffer.c_str();

			if (hisBlkPair._buffer.size() != (sizeof(HisTransBlockV2) + tBlockV2->_size))
			{
				if (_sink) _sink->reader_log(LL_ERROR, "��ʷ��ʳɽ������ļ�%s��СУ��ʧ��", filename.c_str());
				return NULL;
			}

			//��Ҫ��ѹ
			std::string buf = WTSCmpHelper::uncompress_data(tBlockV2->_data, (uint32_t)tBlockV2->_size);

			//��ԭ����bufferֻ����һ��ͷ��,��������tick����׷�ӵ�β��
			hisBlkPair._buffer.resize(sizeof(HisTransBlock));
			hisBlkPair._buffer.append(buf);
			tBlockV2->_version = BLOCK_VERSION_RAW;

			hisBlkPair._block = (HisTransBlock*)hisBlkPair._buffer.c_str();
		}

		HisTransBlockPair& tBlkPair = _his_trans_map[key];
		if (tBlkPair._block == NULL)
			return NULL;

		HisTransBlock* tBlock = tBlkPair._block;

		uint32_t tcnt = (tBlkPair._buffer.size() - sizeof(HisTransBlock)) / sizeof(WTSTransStruct);
		if (tcnt <= 0)
			return NULL;

		WTSTransStruct* pTick = std::lower_bound(tBlock->_items, tBlock->_items + (tcnt - 1), eTick, [](const WTSTransStruct& a, const WTSTransStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pTick - tBlock->_items;
		if (pTick->action_date > eTick.action_date || pTick->action_time >= eTick.action_time)
		{
			pTick--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSTransSlice* slice = WTSTransSlice::create(stdCode, tBlock->_items + sIdx, cnt);
		return slice;
	}
}


bool WtDataReader::cacheFinalBarsFromLoader(const std::string& key, const char* stdCode, WTSKlinePeriod period)
{
	if (NULL == _loader)
		return false;

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	std::string stdPID = StrUtil::printf("%s.%s", cInfo._exchg, cInfo._product);

	BarsList& barList = _bars_cache[key];
	barList._code = stdCode;
	barList._period = period;
	barList._exchg = cInfo._exchg;

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "m1"; break;
	case KP_Minute5: pname = "m5"; break;
	case KP_DAY: pname = "d"; break;
	default: pname = ""; break;
	}

	if(_sink) _sink->reader_log(LL_INFO, "Reading final bars of %s via extended loader...", stdCode);
	bool ret = _loader->loadFinalHisBars(&barList, stdCode, period, [](void* obj, WTSBarStruct* firstBar, uint32_t count) {
		BarsList* bars = (BarsList*)obj;
		bars->_factor = 1.0;
		bars->_bars.resize(count);
		memcpy(bars->_bars.data(), firstBar, sizeof(WTSBarStruct)*count);
	});

	if(ret)
		if (_sink) _sink->reader_log(LL_INFO, "%s items of back {} data of {} loaded via extended loader", barList._bars.size(), pname.c_str(), stdCode);

	return ret;
}


bool WtDataReader::cacheIntegratedFutBars(const std::string& key, const char* stdCode, WTSKlinePeriod period)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	std::string stdPID = StrUtil::printf("%s.%s", cInfo._exchg, cInfo._product);

	uint32_t curDate = TimeUtils::getCurDate();
	uint32_t curTime = TimeUtils::getCurMin() / 100;

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), curDate, curTime, false);

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "min1"; break;
	case KP_Minute5: pname = "min5"; break;
	default: pname = "day"; break;
	}

	BarsList& barList = _bars_cache[key];
	barList._code = stdCode;
	barList._period = period;
	barList._exchg = cInfo._exchg;

	std::vector<std::vector<WTSBarStruct>*> barsSections;

	uint32_t realCnt = 0;

	const char* hot_flag = cInfo.isHot() ? "HOT" : "2ND";

	//�Ȱ���HOT������ж�ȡ, ��rb.HOT
	std::vector<WTSBarStruct>* hotAy = NULL;
	uint32_t lastHotTime = 0;
	do
	{
		/*
		 *	By Wesley @ 2021.12.20
		 *	����������Ҫ�ȵ���_loader->loadRawHisBars���ⲿ��������ȡ������Լ���ݵ�
		 *	�����ϲ�����һ��loadFinalHisBars�������ٵ���loadRawHisBars�������ˣ�����ֱ������
		 */
		std::stringstream ss;
		ss << _base_dir << "his/" << pname << "/" << cInfo._exchg << "/" << cInfo._exchg << "." << cInfo._product << "_" << hot_flag << ".dsb";
		std::string filename = ss.str();
		if (!StdFile::exists(filename.c_str()))
			break;

		std::string content;
		StdFile::read_file_content(filename.c_str(), content);
		if (content.size() < sizeof(HisKlineBlock))
		{
			if (_sink) _sink->reader_log(LL_ERROR, "��ʷK�������ļ�%s��СУ��ʧ��", filename.c_str());
			break;
		}

		HisKlineBlock* kBlock = (HisKlineBlock*)content.c_str();
		std::string buffer;
		if (kBlock->_version == BLOCK_VERSION_CMP)
		{
			if (content.size() < sizeof(HisKlineBlockV2))
			{
				if (_sink) _sink->reader_log(LL_ERROR, "��ʷK�������ļ�%s��СУ��ʧ��", filename.c_str());
				break;
			}

			HisKlineBlockV2* kBlockV2 = (HisKlineBlockV2*)content.c_str();
			if (kBlockV2->_size == 0)
				break;

			buffer = WTSCmpHelper::uncompress_data(kBlockV2->_data, (uint32_t)kBlockV2->_size);
		}
		else
		{
			content.erase(0, sizeof(HisKlineBlock));
			buffer.swap(content);
		}

		uint32_t barcnt = buffer.size() / sizeof(WTSBarStruct);
		if (barcnt <= 0)
			break;

		hotAy = new std::vector<WTSBarStruct>();
		hotAy->resize(barcnt);
		memcpy(hotAy->data(), buffer.data(), buffer.size());

		if (period != KP_DAY)
			lastHotTime = hotAy->at(barcnt - 1).time;
		else
			lastHotTime = hotAy->at(barcnt - 1).date;

		if (_sink) _sink->reader_log(LL_INFO, "%u items of back %s data of wrapped contract %s directly loaded", barcnt, pname.c_str(), stdCode);
	} while (false);

	HotSections secs;
	if (cInfo.isHot())
	{
		if (!_hot_mgr->splitHotSecions(cInfo._exchg, cInfo._product, 19900102, endTDate, secs))
			return false;
	}
	else if (cInfo.isSecond())
	{
		if (!_hot_mgr->splitSecondSecions(cInfo._exchg, cInfo._product, 19900102, endTDate, secs))
			return false;
	}

	if (secs.empty())
		return false;

	bool bAllCovered = false;
	for (auto it = secs.rbegin(); it != secs.rend(); it++)
	{
		const HotSection& hotSec = *it;
		const char* curCode = hotSec._code.c_str();
		uint32_t rightDt = hotSec._e_date;
		uint32_t leftDt = hotSec._s_date;

		//Ҫ�Ƚ�����ת��Ϊ�߽�ʱ��
		WTSBarStruct sBar, eBar;
		if (period != KP_DAY)
		{
			uint64_t sTime = _base_data_mgr->getBoundaryTime(stdPID.c_str(), leftDt, false, true);
			uint64_t eTime = _base_data_mgr->getBoundaryTime(stdPID.c_str(), rightDt, false, false);

			sBar.date = leftDt;
			sBar.time = ((uint32_t)(sTime / 10000) - 19900000) * 10000 + (uint32_t)(sTime % 10000);

			if (sBar.time < lastHotTime)	//����߽�ʱ��С�����������һ��Bar��ʱ��, ˵���Ѿ��н�����, ����Ҫ�ٴ�����
			{
				bAllCovered = true;
				sBar.time = lastHotTime + 1;
			}

			eBar.date = rightDt;
			eBar.time = ((uint32_t)(eTime / 10000) - 19900000) * 10000 + (uint32_t)(eTime % 10000);

			if (eBar.time <= lastHotTime)	//�ұ߽�ʱ��С�����һ��Hotʱ��, ˵��ȫ��������, û�����ҵı�Ҫ��
				break;
		}
		else
		{
			sBar.date = leftDt;
			if (sBar.date < lastHotTime)	//����߽�ʱ��С�����������һ��Bar��ʱ��, ˵���Ѿ��н�����, ����Ҫ�ٴ�����
			{
				bAllCovered = true;
				sBar.date = lastHotTime + 1;
			}

			eBar.date = rightDt;

			if (eBar.date <= lastHotTime)
				break;
		}

		/*
		 *	By Wesley @ 2021.12.20
		 *	�ȴ�extloader��ȡ���º�Լ��K������
		 *	���û�ж������ٴ��ļ���ȡ
		 */
		bool bLoaded = false;
		std::string buffer;
		if (NULL != _loader)
		{
			std::string wCode = StrUtil::printf("%s.%s.%s", cInfo._exchg, cInfo._product, (char*)curCode + strlen(cInfo._product));
			bLoaded = _loader->loadRawHisBars(&buffer, wCode.c_str(), period, [](void* obj, WTSBarStruct* bars, uint32_t count) {
				std::string* buff = (std::string*)obj;
				buff->resize(sizeof(WTSBarStruct)*count);
				memcpy((void*)buff->c_str(), bars, sizeof(WTSBarStruct)*count);
			});
		}

		if (!bLoaded)
		{
			std::stringstream ss;
			ss << _base_dir << "his/" << pname << "/" << cInfo._exchg << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				continue;

			std::string content;
			StdFile::read_file_content(filename.c_str(), content);
			if (content.size() < sizeof(HisKlineBlock))
			{
				if (_sink) _sink->reader_log(LL_ERROR, "��ʷK�������ļ�%s��СУ��ʧ��", filename.c_str());
				return false;
			}

			HisKlineBlock* kBlock = (HisKlineBlock*)content.c_str();
			WTSBarStruct* firstBar = NULL;
			uint32_t barcnt = 0;
			if (kBlock->_version == BLOCK_VERSION_CMP)
			{
				if (content.size() < sizeof(HisKlineBlockV2))
				{
					if (_sink) _sink->reader_log(LL_ERROR, "��ʷK�������ļ�%s��СУ��ʧ��", filename.c_str());
					break;
				}

				HisKlineBlockV2* kBlockV2 = (HisKlineBlockV2*)content.c_str();
				if (kBlockV2->_size == 0)
					break;

				buffer = WTSCmpHelper::uncompress_data(kBlockV2->_data, (uint32_t)kBlockV2->_size);
			}
			else
			{
				content.erase(0, sizeof(HisKlineBlock));
				buffer.swap(content);
			}
		}

		uint32_t barcnt = buffer.size() / sizeof(WTSBarStruct);
		if (barcnt <= 0)
			break;

		WTSBarStruct* firstBar = (WTSBarStruct*)buffer.data();

		WTSBarStruct* pBar = std::lower_bound(firstBar, firstBar + (barcnt - 1), sBar, [period](const WTSBarStruct& a, const WTSBarStruct& b) {
			if (period == KP_DAY)
			{
				return a.date < b.date;
			}
			else
			{
				return a.time < b.time;
			}
		});

		uint32_t sIdx = pBar - firstBar;
		if ((period == KP_DAY && pBar->date < sBar.date) || (period != KP_DAY && pBar->time < sBar.time))	//���ڱ߽�ʱ��
		{
			//���ڱ߽�ʱ��, ˵��û��������, ��Ϊlower_bound�᷵�ش��ڵ���Ŀ��λ�õ�����
			continue;
		}

		pBar = std::lower_bound(firstBar + sIdx, firstBar + (barcnt - 1), eBar, [period](const WTSBarStruct& a, const WTSBarStruct& b) {
			if (period == KP_DAY)
			{
				return a.date < b.date;
			}
			else
			{
				return a.time < b.time;
			}
		});
		uint32_t eIdx = pBar - firstBar;
		if ((period == KP_DAY && pBar->date > eBar.date) || (period != KP_DAY && pBar->time > eBar.time))
		{
			pBar--;
			eIdx--;
		}

		if (eIdx < sIdx)
			continue;

		uint32_t curCnt = eIdx - sIdx + 1;
		std::vector<WTSBarStruct>* tempAy = new std::vector<WTSBarStruct>();
		tempAy->resize(curCnt);
		memcpy(tempAy->data(), &firstBar[sIdx], sizeof(WTSBarStruct)*curCnt);
		realCnt += curCnt;

		barsSections.emplace_back(tempAy);

		if (bAllCovered)
			break;
	}

	if (hotAy)
	{
		barsSections.emplace_back(hotAy);
		realCnt += hotAy->size();
	}

	if (realCnt > 0)
	{
		barList._bars.resize(realCnt);

		uint32_t curIdx = 0;
		for (auto it = barsSections.rbegin(); it != barsSections.rend(); it++)
		{
			std::vector<WTSBarStruct>* tempAy = *it;
			memcpy(barList._bars.data() + curIdx, tempAy->data(), tempAy->size() * sizeof(WTSBarStruct));
			curIdx += tempAy->size();
			delete tempAy;
		}
		barsSections.clear();
	}

	if (_sink) _sink->reader_log(LL_INFO, "%u items of back %s data of %s cached", realCnt, pname.c_str(), stdCode);

	return true;
}

bool WtDataReader::cacheAdjustedStkBars(const std::string& key, const char* stdCode, WTSKlinePeriod period)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	std::string stdPID = StrUtil::printf("%s.%s", cInfo._exchg, cInfo._product);

	uint32_t curDate = TimeUtils::getCurDate();
	uint32_t curTime = TimeUtils::getCurMin() / 100;

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), curDate, curTime, false);

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "min1"; break;
	case KP_Minute5: pname = "min5"; break;
	default: pname = "day"; break;
	}

	BarsList& barList = _bars_cache[key];
	barList._code = stdCode;
	barList._period = period;
	barList._exchg = cInfo._exchg;

	std::vector<std::vector<WTSBarStruct>*> barsSections;

	uint32_t realCnt = 0;

	std::vector<WTSBarStruct>* ayAdjusted = NULL;
	uint32_t lastQTime = 0;

	do
	{
		/*
		 *	By Wesley @ 2021.12.20
		 *	����������Ҫ�ȵ���_loader->loadRawHisBars���ⲿ��������ȡ��Ȩ���ݵ�
		 *	�����ϲ�����һ��loadFinalHisBars�������ٵ���loadRawHisBars�������ˣ�����ֱ������
		 */
		char flag = cInfo._exright == 1 ? 'Q' : 'H';
		std::stringstream ss;
		ss << _base_dir << "his/" << pname << "/" << cInfo._exchg << "/" << cInfo._code << flag << ".dsb";
		std::string filename = ss.str();
		if (!StdFile::exists(filename.c_str()))
			break;

		std::string content;
		StdFile::read_file_content(filename.c_str(), content);
		if (content.size() < sizeof(HisKlineBlock))
		{
			if (_sink) _sink->reader_log(LL_ERROR, "��ʷK�������ļ�%s��СУ��ʧ��", filename.c_str());
			break;
		}

		HisKlineBlock* kBlock = (HisKlineBlock*)content.c_str();
		std::string buffer;
		if (kBlock->_version == BLOCK_VERSION_CMP)
		{
			if (content.size() < sizeof(HisKlineBlockV2))
			{
				if (_sink) _sink->reader_log(LL_ERROR, "��ʷK�������ļ�%s��СУ��ʧ��", filename.c_str());
				break;
			}

			HisKlineBlockV2* kBlockV2 = (HisKlineBlockV2*)content.c_str();
			if (kBlockV2->_size == 0)
				break;

			buffer = WTSCmpHelper::uncompress_data(kBlockV2->_data, (uint32_t)kBlockV2->_size);
		}
		else
		{
			content.erase(0, sizeof(HisKlineBlock));
			buffer.swap(content);
		}

		uint32_t barcnt = buffer.size() / sizeof(WTSBarStruct);
		if (barcnt <= 0)
			break;

		ayAdjusted = new std::vector<WTSBarStruct>();
		ayAdjusted->resize(barcnt);
		memcpy(ayAdjusted->data(), buffer.data(), buffer.size());


		if (period != KP_DAY)
			lastQTime = ayAdjusted->at(barcnt - 1).time;
		else
			lastQTime = ayAdjusted->at(barcnt - 1).date;

		if (_sink) _sink->reader_log(LL_INFO, "%u items of adjusted back %s data of stock %s directly loaded", barcnt, pname.c_str(), stdCode);
	} while (false);


	bool bAllCovered = false;
	do
	{
		const char* curCode = cInfo._code;

		//Ҫ�Ƚ�����ת��Ϊ�߽�ʱ��
		WTSBarStruct sBar;
		if (period != KP_DAY)
		{
			sBar.date = TimeUtils::minBarToDate(lastQTime);

			sBar.time = lastQTime + 1;
		}
		else
		{
			sBar.date = lastQTime + 1;
		}

		/*
		 *	By Wesley @ 2021.12.20
		 *	�ȴ�extloader��ȡ
		 *	���û�ж������ٴ��ļ���ȡ
		 */
		bool bLoaded = false;
		std::string buffer;
		std::string rawCode = StrUtil::printf("%s.%s.%s", cInfo._exchg, cInfo._product, curCode);
		if (NULL != _loader)
		{
			bLoaded = _loader->loadRawHisBars(&buffer, rawCode.c_str(), period, [](void* obj, WTSBarStruct* bars, uint32_t count) {
				std::string* buff = (std::string*)obj;
				buff->resize(sizeof(WTSBarStruct)*count);
				memcpy((void*)buff->c_str(), bars, sizeof(WTSBarStruct)*count);
			});
		}

		if (!bLoaded)
		{
			std::stringstream ss;
			ss << _base_dir << "his/" << pname << "/" << cInfo._exchg << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				continue;

			std::string content;
			StdFile::read_file_content(filename.c_str(), content);
			if (content.size() < sizeof(HisKlineBlock))
			{
				if (_sink) _sink->reader_log(LL_ERROR, "��ʷK�������ļ�%s��СУ��ʧ��", filename.c_str());
				return false;
			}

			HisKlineBlock* kBlock = (HisKlineBlock*)content.c_str();
			WTSBarStruct* firstBar = NULL;
			uint32_t barcnt = 0;
			if (kBlock->_version == BLOCK_VERSION_CMP)
			{
				if (content.size() < sizeof(HisKlineBlockV2))
				{
					if (_sink) _sink->reader_log(LL_ERROR, "��ʷK�������ļ�%s��СУ��ʧ��", filename.c_str());
					break;
				}

				HisKlineBlockV2* kBlockV2 = (HisKlineBlockV2*)content.c_str();
				if (kBlockV2->_size == 0)
					break;

				buffer = WTSCmpHelper::uncompress_data(kBlockV2->_data, (uint32_t)kBlockV2->_size);
			}
			else
			{
				content.erase(0, sizeof(HisKlineBlock));
				buffer.swap(content);
			}
		}

		uint32_t barcnt = buffer.size() / sizeof(WTSBarStruct);
		if (barcnt <= 0)
			break;

		WTSBarStruct* firstBar = (WTSBarStruct*)buffer.data();

		WTSBarStruct* pBar = std::lower_bound(firstBar, firstBar + (barcnt - 1), sBar, [period](const WTSBarStruct& a, const WTSBarStruct& b) {
			if (period == KP_DAY)
			{
				return a.date < b.date;
			}
			else
			{
				return a.time < b.time;
			}
		});

		if (pBar != NULL)
		{
			uint32_t sIdx = pBar - firstBar;
			uint32_t curCnt = barcnt - sIdx;

			std::vector<WTSBarStruct>* ayRaw = new std::vector<WTSBarStruct>();
			ayRaw->resize(curCnt);
			memcpy(ayRaw->data(), &firstBar[sIdx], sizeof(WTSBarStruct)*curCnt);
			realCnt += curCnt;

			auto& ayFactors = getAdjFactors(cInfo._code, cInfo._exchg, cInfo._product);
			if (!ayFactors.empty())
			{
				//����Ȩ����
				int32_t lastIdx = curCnt;
				WTSBarStruct bar;
				firstBar = ayRaw->data();

				//���ݸ�Ȩ����ȷ����������
				//�����ǰ��Ȩ������ʷ���ݻ��С�������һ����Ȩ����Ϊ��������
				//����Ǻ�Ȩ���������ݻ��󣬻�������Ϊ1
				double baseFactor = 1.0;
				if (cInfo._exright == 1)
					baseFactor = ayFactors.back()._factor;
				else if (cInfo._exright == 2)
					barList._factor = ayFactors.back()._factor;

				for (auto it = ayFactors.rbegin(); it != ayFactors.rend(); it++)
				{
					const AdjFactor& adjFact = *it;
					bar.date = adjFact._date;

					//��������
					double factor = adjFact._factor / baseFactor;

					WTSBarStruct* pBar = NULL;
					pBar = std::lower_bound(firstBar, firstBar + lastIdx - 1, bar, [period](const WTSBarStruct& a, const WTSBarStruct& b) {
						return a.date < b.date;
					});

					if (pBar->date < bar.date)
						continue;

					WTSBarStruct* endBar = pBar;
					if (pBar != NULL)
					{
						int32_t curIdx = pBar - firstBar;
						while (pBar && curIdx < lastIdx)
						{
							pBar->open *= factor;
							pBar->high *= factor;
							pBar->low *= factor;
							pBar->close *= factor;

							pBar++;
							curIdx++;
						}
						lastIdx = endBar - firstBar;
					}

					if (lastIdx == 0)
						break;
				}
			}

			barsSections.emplace_back(ayRaw);
		}
	} while (false);

	if (ayAdjusted)
	{
		barsSections.emplace_back(ayAdjusted);
		realCnt += ayAdjusted->size();
	}

	if (realCnt > 0)
	{
		barList._bars.resize(realCnt);

		uint32_t curIdx = 0;
		for (auto it = barsSections.rbegin(); it != barsSections.rend(); it++)
		{
			std::vector<WTSBarStruct>* tempAy = *it;
			memcpy(barList._bars.data() + curIdx, tempAy->data(), tempAy->size() * sizeof(WTSBarStruct));
			curIdx += tempAy->size();
			delete tempAy;
		}
		barsSections.clear();
	}

	if (_sink) _sink->reader_log(LL_INFO, "%u items of back %s data of %s cached", realCnt, pname.c_str(), stdCode);

	return true;
}

bool WtDataReader::cacheHisBarsFromFile(const std::string& key, const char* stdCode, WTSKlinePeriod period)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	std::string stdPID = StrUtil::printf("%s.%s", cInfo._exchg, cInfo._product);

	uint32_t curDate = TimeUtils::getCurDate();
	uint32_t curTime = TimeUtils::getCurMin() / 100;

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), curDate, curTime, false);

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "min1"; break;
	case KP_Minute5: pname = "min5"; break;
	default: pname = "day"; break;
	}

	BarsList& barList = _bars_cache[key];
	barList._code = stdCode;
	barList._period = period;
	barList._exchg = cInfo._exchg;

	std::vector<std::vector<WTSBarStruct>*> barsSections;

	uint32_t realCnt = 0;
	if (!cInfo.isFlat() && cInfo.isFuture())
	{
		//����Ƕ�ȡ�ڻ�������������
		return cacheIntegratedFutBars(key, stdCode, period);
	}
	else if(cInfo.isExright() && cInfo.isStock())
	{
		//����Ƕ�ȡ��Ʊ��Ȩ����
		return cacheAdjustedStkBars(key, stdCode, period);
	}

	
	//ֱ��ԭʼ����ֱ�Ӽ���

	/*
	 *	By Wesley @ 2021.12.20
	 *	�ȴ�extloader��ȡ
	 *	���û�ж������ٴ��ļ���ȡ
	 */
	bool bLoaded = false;
	std::string buffer;
	if (NULL != _loader)
	{
		bLoaded = _loader->loadRawHisBars(&buffer, stdCode, period, [](void* obj, WTSBarStruct* bars, uint32_t count) {
			std::string* buff = (std::string*)obj;
			buff->resize(sizeof(WTSBarStruct)*count);
			memcpy((void*)buff->c_str(), bars, sizeof(WTSBarStruct)*count);
		});
	}

	if (!bLoaded)
	{
		//��ȡ��ʷ��
		std::stringstream ss;
		ss << _base_dir << "his/" << pname << "/" << cInfo._exchg << "/" << cInfo._code << ".dsb";
		std::string filename = ss.str();
		if (StdFile::exists(filename.c_str()))
		{
			//����и�ʽ������ʷ�����ļ�, ��ֱ�Ӷ�ȡ
			std::string content;
			StdFile::read_file_content(filename.c_str(), content);
			if (content.size() < sizeof(HisKlineBlock))
			{
				if (_sink) _sink->reader_log(LL_ERROR, "��ʷK�������ļ�%s��СУ��ʧ��", filename.c_str());
				return false;
			}

			HisKlineBlock* kBlock = (HisKlineBlock*)content.c_str();
			WTSBarStruct* firstBar = NULL;
			uint32_t barcnt = 0;
			if (kBlock->_version == BLOCK_VERSION_CMP)
			{
				if (content.size() < sizeof(HisKlineBlockV2))
				{
					if (_sink) _sink->reader_log(LL_ERROR, "��ʷK�������ļ�%s��СУ��ʧ��", filename.c_str());
					return false;
				}

				HisKlineBlockV2* kBlockV2 = (HisKlineBlockV2*)content.c_str();
				if (kBlockV2->_size == 0)
					return false;

				buffer = WTSCmpHelper::uncompress_data(kBlockV2->_data, (uint32_t)kBlockV2->_size);
			}
			else
			{
				content.erase(0, sizeof(HisKlineBlock));
				buffer.swap(content);
			}
		}
	}

	uint32_t barcnt = buffer.size() / sizeof(WTSBarStruct);
	if (barcnt <= 0)
		return false;

	WTSBarStruct* firstBar = (WTSBarStruct*)buffer.data();

	if (barcnt > 0)
	{

		uint32_t sIdx = 0;
		uint32_t idx = barcnt - 1;
		uint32_t curCnt = (idx - sIdx + 1);

		std::vector<WTSBarStruct>* tempAy = new std::vector<WTSBarStruct>();
		tempAy->resize(curCnt);
		memcpy(tempAy->data(), &firstBar[sIdx], sizeof(WTSBarStruct)*curCnt);
		realCnt += curCnt;

		barsSections.emplace_back(tempAy);
	}

	if (realCnt > 0)
	{
		barList._bars.resize(realCnt);

		uint32_t curIdx = 0;
		for (auto it = barsSections.rbegin(); it != barsSections.rend(); it++)
		{
			std::vector<WTSBarStruct>* tempAy = *it;
			memcpy(barList._bars.data() + curIdx, tempAy->data(), tempAy->size()*sizeof(WTSBarStruct));
			curIdx += tempAy->size();
			delete tempAy;
		}
		barsSections.clear();
	}

	if (_sink) _sink->reader_log(LL_INFO, "%u items of back %s data of %s cached", realCnt, pname.c_str(), stdCode);
	return true;
}

WTSKlineSlice* WtDataReader::readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	std::string stdPID = StrUtil::printf("%s.%s", cInfo._exchg, cInfo._product);

	std::string key = StrUtil::printf("%s#%u", stdCode, period);
	auto it = _bars_cache.find(key);
	bool bHasHisData = false;
	if (it == _bars_cache.end())
	{
		/*
		 *	By Wesley @ 2021.12.20
		 *	�ȴ�extloader�������յ�K�����ݣ�����Ǹ�Ȩ��
		 *	�������ʧ�ܣ����ٴ��ļ�����K������
		 */
		bHasHisData = cacheFinalBarsFromLoader(key, stdCode, period);

		if(!bHasHisData)
			bHasHisData = cacheHisBarsFromFile(key, stdCode, period);
	}
	else
	{
		bHasHisData = true;
	}

	uint32_t curDate, curTime;
	if (etime == 0)
	{
		curDate = _sink->get_date();
		curTime = _sink->get_min_time();
		etime = (uint64_t)curDate * 10000 + curTime;
	}
	else
	{
		curDate = (uint32_t)(etime / 10000);
		curTime = (uint32_t)(etime % 10000);
	}

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), curDate, curTime, false);
	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), 0, 0, false);
	
	WTSBarStruct* hisHead = NULL;
	WTSBarStruct* rtHead = NULL;
	uint32_t hisCnt = 0;
	uint32_t rtCnt = 0;

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "min1"; break;
	case KP_Minute5: pname = "min5"; break;
	default: pname = "day"; break;
	}

	uint32_t left = count;

	//�Ƿ���������
	bool bHasToday = (endTDate == curTDate);

	if (cInfo.isHot() && cInfo.isFuture())
	{
		_bars_cache[key]._raw_code = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, curTDate);
		if (_sink) _sink->reader_log(LL_INFO, "Hot contract of %u confirmed: %s -> %s", curTDate, stdCode, _bars_cache[key]._raw_code.c_str());
	}
	else if (cInfo.isSecond() && cInfo.isFuture())
	{
		_bars_cache[key]._raw_code = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, curTDate);
		if (_sink) _sink->reader_log(LL_INFO, "Second contract of %u confirmed: %s -> %s", curTDate, stdCode, _bars_cache[key]._raw_code.c_str());
	}
	else
	{
		_bars_cache[key]._raw_code = cInfo._code;
	}

	if (bHasToday)
	{
		WTSBarStruct bar;
		bar.date = curDate;
		bar.time = (curDate - 19900000) * 10000 + curTime;

		const char* curCode = _bars_cache[key]._raw_code.c_str();

		//��ȡʵʱ��
		RTKlineBlockPair* kPair = getRTKilneBlock(cInfo._exchg, curCode, period);
		if (kPair != NULL)
		{
			//��ȡ���յ�����
			WTSBarStruct* pBar = std::lower_bound(kPair->_block->_bars, kPair->_block->_bars + (kPair->_block->_size - 1), bar, [period](const WTSBarStruct& a, const WTSBarStruct& b){
				if (period == KP_DAY)
					return a.date < b.date;
				else
					return a.time < b.time;
			});
			uint32_t idx = pBar - kPair->_block->_bars;
			if ((period == KP_DAY && pBar->date > bar.date) || (period != KP_DAY && pBar->time > bar.time))
			{
				pBar--;
				idx--;
			}

			uint32_t sIdx = 0;
			if (left <= idx + 1)
			{
				sIdx = idx - left + 1;
			}

			uint32_t curCnt = (idx - sIdx + 1);
			left -= (idx - sIdx + 1);

			if(cInfo._exright == 2 && cInfo.isStock())
			{
				//��Ȩ����Ҫ�����µ����ݽ��и�Ȩ��������Ҫ��Ϊ��ʷ����׷�ӵ�β��
				BarsList& barsList = _bars_cache[key];
				double factor = barsList._factor;
				uint32_t oldSize = barsList._bars.size();
				uint32_t newSize = oldSize + curCnt;
				barsList._bars.resize(newSize);
				memcpy(&barsList._bars[oldSize], &kPair->_block->_bars[sIdx], sizeof(WTSBarStruct)*curCnt);
				for(uint32_t thisIdx = oldSize; thisIdx < newSize; thisIdx++)
				{
					WTSBarStruct* pBar = &barsList._bars[thisIdx];
					pBar->open *= factor;
					pBar->high *= factor;
					pBar->low *= factor;
					pBar->close *= factor;
				}
			}
			else
			{
				_bars_cache[key]._rt_cursor = idx;				
				rtHead = &kPair->_block->_bars[sIdx];
				rtCnt = curCnt;
			}
			
		}
	}

	if (left > 0 && bHasHisData)
	{
		hisCnt = left;
		//��ʷ���ݣ�ֱ�Ӵӻ������ʷ����β����ȡ
		BarsList& barList = _bars_cache[key];
		hisCnt = min(hisCnt, (uint32_t)barList._bars.size());
		hisHead = &barList._bars[barList._bars.size() - hisCnt];//indexBarFromCache(key, etime, hisCnt, period == KP_DAY);
	}

	if (hisCnt + rtCnt > 0)
	{
		WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, 1, hisHead, hisCnt, rtHead, rtCnt);
		return slice;
	}

	return NULL;
}


WtDataReader::TickBlockPair* WtDataReader::getRTTickBlock(const char* exchg, const char* code)
{
	std::string key = StrUtil::printf("%s.%s", exchg, code);

	std::string path = StrUtil::printf("%srt/ticks/%s/%s.dmb", _base_dir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return NULL;

	TickBlockPair& block = _rt_tick_map[key];
	if (block._file == NULL || block._block == NULL)
	{
		if (block._file == NULL)
		{
			block._file.reset(new BoostMappingFile());
		}

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTTickBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}
	else if (block._last_cap != block._block->_capacity)
	{
		//˵���ļ���С�ѱ�, ��Ҫ����ӳ��
		block._file.reset(new BoostMappingFile());
		block._last_cap = 0;
		block._block = NULL;

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTTickBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}

	return &block;
}

WtDataReader::OrdDtlBlockPair* WtDataReader::getRTOrdDtlBlock(const char* exchg, const char* code)
{
	std::string key = StrUtil::printf("%s.%s", exchg, code);

	std::string path = StrUtil::printf("%srt/orders/%s/%s.dmb", _base_dir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return NULL;

	OrdDtlBlockPair& block = _rt_orddtl_map[key];
	if (block._file == NULL || block._block == NULL)
	{
		if (block._file == NULL)
		{
			block._file.reset(new BoostMappingFile());
		}

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTOrdDtlBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}
	else if (block._last_cap != block._block->_capacity)
	{
		//˵���ļ���С�ѱ�, ��Ҫ����ӳ��
		block._file.reset(new BoostMappingFile());
		block._last_cap = 0;
		block._block = NULL;

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTOrdDtlBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}

	return &block;
}

WtDataReader::OrdQueBlockPair* WtDataReader::getRTOrdQueBlock(const char* exchg, const char* code)
{
	std::string key = StrUtil::printf("%s.%s", exchg, code);

	std::string path = StrUtil::printf("%srt/queue/%s/%s.dmb", _base_dir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return NULL;

	OrdQueBlockPair& block = _rt_ordque_map[key];
	if (block._file == NULL || block._block == NULL)
	{
		if (block._file == NULL)
		{
			block._file.reset(new BoostMappingFile());
		}

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTOrdQueBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}
	else if (block._last_cap != block._block->_capacity)
	{
		//˵���ļ���С�ѱ�, ��Ҫ����ӳ��
		block._file.reset(new BoostMappingFile());
		block._last_cap = 0;
		block._block = NULL;

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTOrdQueBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}

	return &block;
}

WtDataReader::TransBlockPair* WtDataReader::getRTTransBlock(const char* exchg, const char* code)
{
	std::string key = StrUtil::printf("%s.%s", exchg, code);

	std::string path = StrUtil::printf("%srt/trans/%s/%s.dmb", _base_dir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return NULL;

	TransBlockPair& block = _rt_trans_map[key];
	if (block._file == NULL || block._block == NULL)
	{
		if (block._file == NULL)
		{
			block._file.reset(new BoostMappingFile());
		}

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTTransBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}
	else if (block._last_cap != block._block->_capacity)
	{
		//˵���ļ���С�ѱ�, ��Ҫ����ӳ��
		block._file.reset(new BoostMappingFile());
		block._last_cap = 0;
		block._block = NULL;

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTTransBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}

	return &block;
}

WtDataReader::RTKlineBlockPair* WtDataReader::getRTKilneBlock(const char* exchg, const char* code, WTSKlinePeriod period)
{
	if (period != KP_Minute1 && period != KP_Minute5)
		return NULL;

	std::string key = StrUtil::printf("%s.%s", exchg, code);

	RTKBlockFilesMap* cache_map = NULL;
	std::string subdir = "";
	BlockType bType;
	switch (period)
	{
	case KP_Minute1:
		cache_map = &_rt_min1_map;
		subdir = "min1";
		bType = BT_RT_Minute1;
		break;
	case KP_Minute5:
		cache_map = &_rt_min5_map;
		subdir = "min5";
		bType = BT_RT_Minute5;
		break;
	default: break;
	}

	std::string path = StrUtil::printf("%srt/%s/%s/%s.dmb", _base_dir.c_str(), subdir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return NULL;

	RTKlineBlockPair& block = (*cache_map)[key];
	if (block._file == NULL || block._block == NULL)
	{
		if (block._file == NULL)
		{
			block._file.reset(new BoostMappingFile());
		}

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTKlineBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}
	else if (block._last_cap != block._block->_capacity)
	{
		//˵���ļ���С�ѱ�, ��Ҫ����ӳ��
		block._file.reset(new BoostMappingFile());
		block._last_cap = 0;
		block._block = NULL;

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTKlineBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}

	return &block;
}

void WtDataReader::onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate /* = 0 */)
{
	//����Ӧ�ô������
	uint64_t nowTime = (uint64_t)uDate * 10000 + uTime;
	if (nowTime <= _last_time)
		return;

	for (auto it = _bars_cache.begin(); it != _bars_cache.end(); it++)
	{
		BarsList& barsList = (BarsList&)it->second;
		if (barsList._period != KP_DAY)
		{
			if (!barsList._raw_code.empty())
			{
				RTKlineBlockPair* kBlk = getRTKilneBlock(barsList._exchg.c_str(), barsList._raw_code.c_str(), barsList._period);
				if (kBlk == NULL)
					continue;

				uint32_t preCnt = 0;
				if (barsList._rt_cursor == UINT_MAX)
					preCnt = 0;
				else
					preCnt = barsList._rt_cursor + 1;

				for (;;)
				{
					if (kBlk->_block->_size <= preCnt)
						break;

					WTSBarStruct& nextBar = kBlk->_block->_bars[preCnt];

					uint64_t barTime = 199000000000 + nextBar.time;
					if (barTime <= nowTime)
					{
						//������Ǻ�Ȩ����ֱ�ӻص�onbar
						//����Ǻ�Ȩ��������bar��Ȩ�����Ժ���ӵ�cache�У��ٻص�onbar
						if(barsList._factor == DBL_MAX)
						{
							_sink->on_bar(barsList._code.c_str(), barsList._period, &nextBar);
						}
						else
						{
							WTSBarStruct cpBar = nextBar;
							cpBar.open *= barsList._factor;
							cpBar.high *= barsList._factor;
							cpBar.low *= barsList._factor;
							cpBar.close *= barsList._factor;

							barsList._bars.emplace_back(cpBar);

							_sink->on_bar(barsList._code.c_str(), barsList._period, &barsList._bars[barsList._bars.size()-1]);
						}
					}
					else
					{
						break;
					}

					preCnt++;
				}

				if (preCnt > 0)
					barsList._rt_cursor = preCnt - 1;
			}
		}
		//��һ���߼�û�����ˣ���ʵ���������ǲ���պϵģ�����Ҳ�����ڵ���K�߱պϵ����
		//ʵ���ж�ͨ��ontick������ʵʱ����
		//else if (barsList._period == KP_DAY)
		//{
		//	if (barsList._his_cursor != UINT_MAX && barsList._bars.size() - 1 > barsList._his_cursor)
		//	{
		//		for (;;)
		//		{
		//			WTSBarStruct& nextBar = barsList._bars[barsList._his_cursor + 1];

		//			if (nextBar.date <= endTDate)
		//			{
		//				_sink->on_bar(barsList._code.c_str(), barsList._period, &nextBar);
		//			}
		//			else
		//			{
		//				break;
		//			}

		//			barsList._his_cursor++;

		//			if (barsList._his_cursor == barsList._bars.size() - 1)
		//				break;
		//		}
		//	}
		//}
	}

	if (_sink)
		_sink->on_all_bar_updated(uTime);

	_last_time = nowTime;
}

double WtDataReader::getAdjFactorByDate(const char* stdCode, uint32_t date /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	if (!cInfo.isStock())
		return 1.0;

	AdjFactor factor = { date, 1.0 };

	std::string key = cInfo.pureStdCode();
	const AdjFactorList& factList = _adj_factors[key];
	if (factList.empty())
		return 1.0;

	auto it = std::lower_bound(factList.begin(), factList.end(), factor, [](const AdjFactor& a, const AdjFactor&b) {
		return a._date < b._date;
	});

	if(it == factList.end())
	{
		//�Ҳ�������˵��Ŀ�����ڴ������һ�������ڣ�ֱ�ӷ������һ����Ȩ����
		return factList.back()._factor;
	}
	else
	{
		//����ҵ��ˣ��������е����ڴ���Ŀ�����ڣ�������һ��
		//�������Ŀ�����ڣ�����������һ��
		if ((*it)._date > date)
			it--;

		return (*it)._factor;
	}
}

const WtDataReader::AdjFactorList& WtDataReader::getAdjFactors(const char* code, const char* exchg, const char* pid)
{
	char key[20] = { 0 };
	sprintf(key, "%s.%s.%s", exchg, pid, code);

	auto it = _adj_factors.find(key);
	if (it == _adj_factors.end())
	{
		//By Wesley @ 2021.12.21
		//���û�и�Ȩ���ӣ��ʹ�extloader�����һ��
		if (_loader)
		{
			if(_sink) _sink->reader_log(LL_INFO, "No adjusting factors of %s cached, searching via extented loader...", key);
			_loader->loadAdjFactors(this, key, [](void* obj, const char* stdCode, uint32_t* dates, double* factors, uint32_t count) {
				WtDataReader* self = (WtDataReader*)obj;
				AdjFactorList& fctrLst = self->_adj_factors[stdCode];

				for (uint32_t i = 0; i < count; i++)
				{
					AdjFactor adjFact;
					adjFact._date = dates[i];
					adjFact._factor = factors[i];

					fctrLst.emplace_back(adjFact);
				}

				//һ��Ҫ�ѵ�һ���ӽ�ȥ����Ȼ�����ǰ��Ȩ�Ļ������ܻ�©�������������
				AdjFactor adjFact;
				adjFact._date = 19900101;
				adjFact._factor = 1;
				fctrLst.emplace_back(adjFact);

				std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right) {
					return left._date < right._date;
				});

				if (self->_sink) self->_sink->reader_log(LL_INFO, "%u items of adjusting factors of %s loaded via extended loader", count, stdCode);
			});
		}
	}

	return _adj_factors[key];
}