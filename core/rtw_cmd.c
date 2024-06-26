/******************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 *
 ******************************************************************************/
#define _RTW_CMD_C_

#include <drv_conf.h>
#include <osdep_service.h>
#include <drv_types.h>
#include <recv_osdep.h>
#include <cmd_osdep.h>
#include <mlme_osdep.h>
#include <rtw_byteorder.h>
#ifdef CONFIG_BR_EXT
#include <rtw_br_ext.h>
#endif //CONFIG_BR_EXT
/*
Caller and the rtw_cmd_thread can protect cmd_q by spin_lock.
No irqsave is necessary.
*/

sint	_rtw_init_cmd_priv (struct	cmd_priv *pcmdpriv)
{
	sint res=_SUCCESS;

_func_enter_;

	_rtw_init_sema(&(pcmdpriv->cmd_queue_sema), 0);
	//_rtw_init_sema(&(pcmdpriv->cmd_done_sema), 0);
	_rtw_init_sema(&(pcmdpriv->terminate_cmdthread_sema), 0);


	_rtw_init_queue(&(pcmdpriv->cmd_queue));

	//allocate DMA-able/Non-Page memory for cmd_buf and rsp_buf

	pcmdpriv->cmd_seq = 1;

	pcmdpriv->cmd_allocated_buf = kzalloc(MAX_CMDSZ + CMDBUFF_ALIGN_SZ, in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);

	if (pcmdpriv->cmd_allocated_buf == NULL){
		res= _FAIL;
		goto exit;
	}

	pcmdpriv->cmd_buf = pcmdpriv->cmd_allocated_buf  +  CMDBUFF_ALIGN_SZ - ( (SIZE_PTR)(pcmdpriv->cmd_allocated_buf) & (CMDBUFF_ALIGN_SZ-1));

	pcmdpriv->rsp_allocated_buf = kzalloc(MAX_RSPSZ + 4, in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);

	if (pcmdpriv->rsp_allocated_buf == NULL){
		res= _FAIL;
		goto exit;
	}

	pcmdpriv->rsp_buf = pcmdpriv->rsp_allocated_buf  +  4 - ( (SIZE_PTR)(pcmdpriv->rsp_allocated_buf) & 3);

	pcmdpriv->cmd_issued_cnt = pcmdpriv->cmd_done_cnt = pcmdpriv->rsp_cnt = 0;

exit:

_func_exit_;

	return res;

}

#ifdef CONFIG_C2H_WK
static void c2h_wk_callback(_workitem *work);
#endif
sint _rtw_init_evt_priv(struct evt_priv *pevtpriv)
{
	sint res=_SUCCESS;

_func_enter_;

#ifdef CONFIG_H2CLBK
	_rtw_init_sema(&(pevtpriv->lbkevt_done), 0);
	pevtpriv->lbkevt_limit = 0;
	pevtpriv->lbkevt_num = 0;
	pevtpriv->cmdevt_parm = NULL;
#endif

	//allocate DMA-able/Non-Page memory for cmd_buf and rsp_buf
	ATOMIC_SET(&pevtpriv->event_seq, 0);
	pevtpriv->evt_done_cnt = 0;

#ifdef CONFIG_EVENT_THREAD_MODE

	_rtw_init_sema(&(pevtpriv->evt_notify), 0);
	_rtw_init_sema(&(pevtpriv->terminate_evtthread_sema), 0);

	pevtpriv->evt_allocated_buf = kzalloc(MAX_EVTSZ + 4, in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (pevtpriv->evt_allocated_buf == NULL){
		res= _FAIL;
		goto exit;
	}
	pevtpriv->evt_buf = pevtpriv->evt_allocated_buf  +  4 - ((unsigned int)(pevtpriv->evt_allocated_buf) & 3);


	_rtw_init_queue(&(pevtpriv->evt_queue));

exit:

#endif //end of CONFIG_EVENT_THREAD_MODE

#ifdef CONFIG_C2H_WK
	_init_workitem(&pevtpriv->c2h_wk, c2h_wk_callback, NULL);
	pevtpriv->c2h_wk_alive = _FALSE;
	pevtpriv->c2h_queue = rtw_cbuf_alloc(C2H_QUEUE_MAX_LEN+1);
#endif

_func_exit_;

	return res;
}

void _rtw_free_evt_priv (struct	evt_priv *pevtpriv)
{
_func_enter_;

	RT_TRACE(_module_rtl871x_cmd_c_,_drv_info_,("+_rtw_free_evt_priv \n"));

#ifdef CONFIG_EVENT_THREAD_MODE
	_rtw_free_sema(&(pevtpriv->evt_notify));
	_rtw_free_sema(&(pevtpriv->terminate_evtthread_sema));


	if (pevtpriv->evt_allocated_buf)
		rtw_mfree(pevtpriv->evt_allocated_buf, MAX_EVTSZ + 4);
#endif

#ifdef CONFIG_C2H_WK
	_cancel_workitem_sync(&pevtpriv->c2h_wk);
	while(pevtpriv->c2h_wk_alive)
		rtw_msleep_os(10);

	while (!rtw_cbuf_empty(pevtpriv->c2h_queue)) {
		void *c2h;
		if ((c2h = rtw_cbuf_pop(pevtpriv->c2h_queue)) != NULL
			&& c2h != (void *)pevtpriv) {
			rtw_mfree(c2h, 16);
		}
	}
	rtw_cbuf_free(pevtpriv->c2h_queue);
#endif

	RT_TRACE(_module_rtl871x_cmd_c_,_drv_info_,("-_rtw_free_evt_priv \n"));

_func_exit_;

}

void _rtw_free_cmd_priv (struct	cmd_priv *pcmdpriv)
{
_func_enter_;

	if(pcmdpriv){
		_rtw_spinlock_free(&(pcmdpriv->cmd_queue.lock));
		_rtw_free_sema(&(pcmdpriv->cmd_queue_sema));
		//_rtw_free_sema(&(pcmdpriv->cmd_done_sema));
		_rtw_free_sema(&(pcmdpriv->terminate_cmdthread_sema));

		if (pcmdpriv->cmd_allocated_buf)
			rtw_mfree(pcmdpriv->cmd_allocated_buf, MAX_CMDSZ + CMDBUFF_ALIGN_SZ);

		if (pcmdpriv->rsp_allocated_buf)
			rtw_mfree(pcmdpriv->rsp_allocated_buf, MAX_RSPSZ + 4);
	}
_func_exit_;
}

/*
Calling Context:

rtw_enqueue_cmd can only be called between kernel thread,
since only spin_lock is used.

ISR/Call-Back functions can't call this sub-function.

*/

sint	_rtw_enqueue_cmd(_queue *queue, struct cmd_obj *obj)
{
	_irqL irqL;

_func_enter_;

	if (obj == NULL)
		goto exit;

	//_enter_critical_bh(&queue->lock, &irqL);
	_enter_critical(&queue->lock, &irqL);

	rtw_list_insert_tail(&obj->list, &queue->queue);

	//_exit_critical_bh(&queue->lock, &irqL);
	_exit_critical(&queue->lock, &irqL);

exit:

_func_exit_;

	return _SUCCESS;
}

struct	cmd_obj	*_rtw_dequeue_cmd(_queue *queue)
{
	_irqL irqL;
	struct cmd_obj *obj;

_func_enter_;

	//_enter_critical_bh(&(queue->lock), &irqL);
	_enter_critical(&queue->lock, &irqL);
	if (rtw_is_list_empty(&(queue->queue)))
		obj = NULL;
	else
	{
		obj = LIST_CONTAINOR(get_next(&(queue->queue)), struct cmd_obj, list);
		rtw_list_delete(&obj->list);
	}

	//_exit_critical_bh(&(queue->lock), &irqL);
	_exit_critical(&queue->lock, &irqL);

_func_exit_;

	return obj;
}

u32	rtw_init_cmd_priv(struct cmd_priv *pcmdpriv)
{
	u32	res;
_func_enter_;
	res = _rtw_init_cmd_priv (pcmdpriv);
_func_exit_;
	return res;
}

u32	rtw_init_evt_priv (struct	evt_priv *pevtpriv)
{
	int	res;
_func_enter_;
	res = _rtw_init_evt_priv(pevtpriv);
_func_exit_;
	return res;
}

void rtw_free_evt_priv (struct	evt_priv *pevtpriv)
{
_func_enter_;
	RT_TRACE(_module_rtl871x_cmd_c_,_drv_info_,("rtw_free_evt_priv\n"));
	_rtw_free_evt_priv(pevtpriv);
_func_exit_;
}

void rtw_free_cmd_priv (struct	cmd_priv *pcmdpriv)
{
_func_enter_;
	RT_TRACE(_module_rtl871x_cmd_c_,_drv_info_,("rtw_free_cmd_priv\n"));
	_rtw_free_cmd_priv(pcmdpriv);
_func_exit_;
}

int rtw_cmd_filter(struct cmd_priv *pcmdpriv, struct cmd_obj *cmd_obj);
int rtw_cmd_filter(struct cmd_priv *pcmdpriv, struct cmd_obj *cmd_obj)
{
	u8 bAllow = _FALSE; //set to _TRUE to allow enqueuing cmd when hw_init_completed is _FALSE

	#ifdef SUPPORT_HW_RFOFF_DETECTED
	//To decide allow or not
	if(	(pcmdpriv->padapter->pwrctrlpriv.bHWPwrPindetect)
		&&(!pcmdpriv->padapter->registrypriv.usbss_enable)
	)
	{
		if(cmd_obj->cmdcode == GEN_CMD_CODE(_Set_Drv_Extra) )
		{
			struct drvextra_cmd_parm	*pdrvextra_cmd_parm = (struct drvextra_cmd_parm	*)cmd_obj->parmbuf;
			if(pdrvextra_cmd_parm->ec_id == POWER_SAVING_CTRL_WK_CID)
			{
				//DBG_871X("==>enqueue POWER_SAVING_CTRL_WK_CID\n");
				bAllow = _TRUE;
			}
		}
	}
	#endif

	if(cmd_obj->cmdcode == GEN_CMD_CODE(_SetChannelPlan))
		bAllow = _TRUE;

	if( (pcmdpriv->padapter->hw_init_completed ==_FALSE && bAllow == _FALSE)
		|| pcmdpriv->cmdthd_running== _FALSE	//com_thread not running
	)
	{
		//DBG_871X("%s:%s: drop cmdcode:%u, hw_init_completed:%u, cmdthd_running:%u\n", caller_func, __FUNCTION__,
		//	cmd_obj->cmdcode,
		//	pcmdpriv->padapter->hw_init_completed,
		//	pcmdpriv->cmdthd_running
		//);

		return _FAIL;
	}
	return _SUCCESS;
}



u32 rtw_enqueue_cmd(struct cmd_priv *pcmdpriv, struct cmd_obj *cmd_obj)
{
	int res = _FAIL;
	PADAPTER padapter = pcmdpriv->padapter;

_func_enter_;

	if (cmd_obj == NULL) {
		goto exit;
	}

	cmd_obj->padapter = padapter;

#ifdef CONFIG_CONCURRENT_MODE
	//change pcmdpriv to primary's pcmdpriv
	if (padapter->adapter_type != PRIMARY_ADAPTER && padapter->pbuddy_adapter)
		pcmdpriv = &(padapter->pbuddy_adapter->cmdpriv);
#endif

	if( _FAIL == (res=rtw_cmd_filter(pcmdpriv, cmd_obj)) ) {
		rtw_free_cmd_obj(cmd_obj);
		goto exit;
	}

	res = _rtw_enqueue_cmd(&pcmdpriv->cmd_queue, cmd_obj);

	if(res == _SUCCESS)
		_rtw_up_sema(&pcmdpriv->cmd_queue_sema);

exit:

_func_exit_;

	return res;
}

struct	cmd_obj	*rtw_dequeue_cmd(struct cmd_priv *pcmdpriv)
{
	struct cmd_obj *cmd_obj;

_func_enter_;

	cmd_obj = _rtw_dequeue_cmd(&pcmdpriv->cmd_queue);

_func_exit_;
	return cmd_obj;
}

void rtw_cmd_clr_isr(struct	cmd_priv *pcmdpriv)
{
_func_enter_;
	pcmdpriv->cmd_done_cnt++;
	//_rtw_up_sema(&(pcmdpriv->cmd_done_sema));
_func_exit_;
}

void rtw_free_cmd_obj(struct cmd_obj *pcmd)
{
_func_enter_;

	if((pcmd->cmdcode!=_JoinBss_CMD_) &&(pcmd->cmdcode!= _CreateBss_CMD_))
	{
		//free parmbuf in cmd_obj
		rtw_mfree((unsigned char*)pcmd->parmbuf, pcmd->cmdsz);
	}

	if(pcmd->rsp!=NULL)
	{
		if(pcmd->rspsz!= 0)
		{
			//free rsp in cmd_obj
			rtw_mfree((unsigned char*)pcmd->rsp, pcmd->rspsz);
		}
	}

	//free cmd_obj
	rtw_mfree((unsigned char*)pcmd, sizeof(struct cmd_obj));

_func_exit_;
}

void rtw_stop_cmd_thread(_adapter *adapter)
{
	if(adapter->cmdThread && adapter->cmdpriv.cmdthd_running == _TRUE
		&& adapter->cmdpriv.stop_req == 0)
	{
		adapter->cmdpriv.stop_req = 1;
		_rtw_up_sema(&adapter->cmdpriv.cmd_queue_sema);
		_rtw_down_sema(&adapter->cmdpriv.terminate_cmdthread_sema);
	}
}

thread_return rtw_cmd_thread(thread_context context)
{
	u8 ret;
	struct cmd_obj *pcmd;
	u8 *pcmdbuf, *prspbuf;
	u32 cmd_start_time;
	u32 cmd_process_time;
	u8 (*cmd_hdl)(_adapter *padapter, u8* pbuf);
	void (*pcmd_callback)(_adapter *dev, struct cmd_obj *pcmd);
       _adapter *padapter = (_adapter *)context;
	struct cmd_priv *pcmdpriv = &(padapter->cmdpriv);

_func_enter_;

	thread_enter("RTW_CMD_THREAD");

	pcmdbuf = pcmdpriv->cmd_buf;
	prspbuf = pcmdpriv->rsp_buf;

	pcmdpriv->stop_req = 0;
	pcmdpriv->cmdthd_running=_TRUE;
	_rtw_up_sema(&pcmdpriv->terminate_cmdthread_sema);

	RT_TRACE(_module_rtl871x_cmd_c_,_drv_info_,("start r871x rtw_cmd_thread !!!!\n"));

	while(1)
	{
		if ((_rtw_down_sema(&(pcmdpriv->cmd_queue_sema))) == _FAIL) {
			LOG_LEVEL(_drv_always_, FUNC_ADPT_FMT" _rtw_down_sema(&pcmdpriv->cmd_queue_sema) return _FAIL, break\n", FUNC_ADPT_ARG(padapter));
			break;
		}

		if (pcmdpriv->stop_req) {
			LOG_LEVEL(_drv_always_, FUNC_ADPT_FMT" stop_req:%u, break\n", FUNC_ADPT_ARG(padapter), pcmdpriv->stop_req);
			break;
		}

#ifdef CONFIG_LPS_LCLK
		if (rtw_register_cmd_alive(padapter) != _SUCCESS)
		{
			continue;
		}
#endif

_next:
		if ((padapter->bDriverStopped == _TRUE)||(padapter->bSurpriseRemoved == _TRUE))
		{
			LOG_LEVEL(_drv_always_, "%s: DriverStopped(%d) SurpriseRemoved(%d) break at line %d\n",
				__FUNCTION__, padapter->bDriverStopped, padapter->bSurpriseRemoved, __LINE__);
			break;
		}

		if(!(pcmd = rtw_dequeue_cmd(pcmdpriv))) {
#ifdef CONFIG_LPS_LCLK
			rtw_unregister_cmd_alive(padapter);
#endif
			continue;
		}

		cmd_start_time = rtw_get_current_time();

		if( _FAIL == rtw_cmd_filter(pcmdpriv, pcmd) )
		{
			pcmd->res = H2C_DROPPED;
			goto post_process;
		}

		if( _FAIL == rtw_cmd_filter(pcmdpriv, pcmd) ) {
			rtw_free_cmd_obj(pcmd);
			continue;
		}

		pcmdpriv->cmd_issued_cnt++;

		pcmd->cmdsz = _RND4((pcmd->cmdsz));//_RND4

		memcpy(pcmdbuf, pcmd->parmbuf, pcmd->cmdsz);

		if(pcmd->cmdcode <= (sizeof(wlancmds) /sizeof(struct cmd_hdl)))
		{
			cmd_hdl = wlancmds[pcmd->cmdcode].h2cfuns;

			if (cmd_hdl)
			{
				ret = cmd_hdl(pcmd->padapter, pcmdbuf);
				pcmd->res = ret;
			}

			pcmdpriv->cmd_seq++;
		}
		else
		{
			pcmd->res = H2C_PARAMETERS_ERROR;
		}

		cmd_hdl = NULL;

post_process:

		if((cmd_process_time = rtw_get_passing_time_ms(cmd_start_time)) > 1000)
		{
			if (pcmd->cmdcode == GEN_CMD_CODE(_Set_Drv_Extra)) {
				struct drvextra_cmd_parm *drvextra_parm = (struct drvextra_cmd_parm *)pcmdbuf;
				DBG_871X(ADPT_FMT" cmd=%d,%d,%d process_time=%d > 1 sec\n",
					ADPT_ARG(pcmd->padapter), pcmd->cmdcode, drvextra_parm->ec_id, drvextra_parm->type_size, cmd_process_time);
				//rtw_warn_on(1);
			} else if(pcmd->cmdcode == GEN_CMD_CODE(_Set_MLME_EVT)){
				struct C2HEvent_Header *pc2h_evt_hdr = (struct C2HEvent_Header *)pcmdbuf;
				DBG_871X(ADPT_FMT" cmd=%d,%d, process_time=%d > 1 sec\n",
					ADPT_ARG(pcmd->padapter), pcmd->cmdcode, pc2h_evt_hdr->ID, cmd_process_time);
				//rtw_warn_on(1);
			} else {
				DBG_871X(ADPT_FMT" cmd=%d, process_time=%d > 1 sec\n",
					ADPT_ARG(pcmd->padapter), pcmd->cmdcode, cmd_process_time);
				//rtw_warn_on(1);
			}
		}

		//call callback function for post-processed
		if(pcmd->cmdcode <= (sizeof(rtw_cmd_callback) /sizeof(struct _cmd_callback)))
		{
			pcmd_callback = rtw_cmd_callback[pcmd->cmdcode].callback;
			if(pcmd_callback == NULL)
			{
				RT_TRACE(_module_rtl871x_cmd_c_,_drv_info_,("mlme_cmd_hdl(): pcmd_callback=0x%p, cmdcode=0x%x\n", pcmd_callback, pcmd->cmdcode));
				rtw_free_cmd_obj(pcmd);
			}
			else
			{
				//todo: !!! fill rsp_buf to pcmd->rsp if (pcmd->rsp!=NULL)
				pcmd_callback(pcmd->padapter, pcmd);//need conider that free cmd_obj in rtw_cmd_callback
			}
		}

		flush_signals_thread();

		goto _next;

	}
	pcmdpriv->cmdthd_running=_FALSE;


	// free all cmd_obj resources
	do{
		pcmd = rtw_dequeue_cmd(pcmdpriv);
		if(pcmd==NULL)
			break;

		//DBG_871X("%s: leaving... drop cmdcode:%u\n", __FUNCTION__, pcmd->cmdcode);

		rtw_free_cmd_obj(pcmd);
	}while(1);

	_rtw_up_sema(&pcmdpriv->terminate_cmdthread_sema);

_func_exit_;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0)
	thread_exit();
#else
	kthread_complete_and_exit(NULL, 0);
#endif

}


#ifdef CONFIG_EVENT_THREAD_MODE
u32 rtw_enqueue_evt(struct evt_priv *pevtpriv, struct evt_obj *obj)
{
	_irqL irqL;
	int	res;
	_queue *queue = &pevtpriv->evt_queue;

_func_enter_;

	res = _SUCCESS;

	if (obj == NULL) {
		res = _FAIL;
		goto exit;
	}

	_enter_critical_bh(&queue->lock, &irqL);

	rtw_list_insert_tail(&obj->list, &queue->queue);

	_exit_critical_bh(&queue->lock, &irqL);

	//rtw_evt_notify_isr(pevtpriv);

exit:

_func_exit_;

	return res;
}

struct evt_obj *rtw_dequeue_evt(_queue *queue)
{
	_irqL irqL;
	struct	evt_obj	*pevtobj;

_func_enter_;

	_enter_critical_bh(&queue->lock, &irqL);

	if (rtw_is_list_empty(&(queue->queue)))
		pevtobj = NULL;
	else
	{
		pevtobj = LIST_CONTAINOR(get_next(&(queue->queue)), struct evt_obj, list);
		rtw_list_delete(&pevtobj->list);
	}

	_exit_critical_bh(&queue->lock, &irqL);

_func_exit_;

	return pevtobj;
}

void rtw_free_evt_obj(struct evt_obj *pevtobj)
{
_func_enter_;

	if(pevtobj->parmbuf)
		rtw_mfree((unsigned char*)pevtobj->parmbuf, pevtobj->evtsz);

	rtw_mfree((unsigned char*)pevtobj, sizeof(struct evt_obj));

_func_exit_;
}

void rtw_evt_notify_isr(struct evt_priv *pevtpriv)
{
_func_enter_;
	pevtpriv->evt_done_cnt++;
	_rtw_up_sema(&(pevtpriv->evt_notify));
_func_exit_;
}
#endif


/*
u8 rtw_setstandby_cmd(unsigned char  *adapter)
*/
u8 rtw_setstandby_cmd(_adapter *padapter, uint action)
{
	struct cmd_obj*			ph2c;
	struct usb_suspend_parm*	psetusbsuspend;
	struct cmd_priv				*pcmdpriv=&padapter->cmdpriv;

	u8 ret = _SUCCESS;

_func_enter_;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (ph2c == NULL) {
		ret = _FAIL;
		goto exit;
	}

	psetusbsuspend = kzalloc(sizeof(struct usb_suspend_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (psetusbsuspend == NULL) {
		rtw_mfree((u8 *) ph2c, sizeof(struct	cmd_obj));
		ret = _FAIL;
		goto exit;
	}

	psetusbsuspend->action = action;

	init_h2fwcmd_w_parm_no_rsp(ph2c, psetusbsuspend, GEN_CMD_CODE(_SetUsbSuspend));

	ret = rtw_enqueue_cmd(pcmdpriv, ph2c);

exit:

_func_exit_;

	return ret;
}

/*
rtw_sitesurvey_cmd(~)
	### NOTE:#### (!!!!)
	MUST TAKE CARE THAT BEFORE CALLING THIS FUNC, YOU SHOULD HAVE LOCKED pmlmepriv->lock
*/
u8 rtw_sitesurvey_cmd(_adapter  *padapter, NDIS_802_11_SSID *ssid, int ssid_num,
	struct rtw_ieee80211_channel *ch, int ch_num)
{
	u8 res = _FAIL;
	struct cmd_obj		*ph2c;
	struct sitesurvey_parm	*psurveyPara;
	struct cmd_priv		*pcmdpriv = &padapter->cmdpriv;
	struct mlme_priv	*pmlmepriv = &padapter->mlmepriv;

_func_enter_;

#ifdef CONFIG_LPS
	if(check_fwstate(pmlmepriv, _FW_LINKED) == _TRUE){
		rtw_lps_ctrl_wk_cmd(padapter, LPS_CTRL_SCAN, 1);
	}
#endif

#ifdef CONFIG_P2P_PS
	if (check_fwstate(pmlmepriv, _FW_LINKED) == _TRUE) {
		p2p_ps_wk_cmd(padapter, P2P_PS_SCAN, 1);
	}
#endif // CONFIG_P2P_PS

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (ph2c == NULL)
		return _FAIL;

	psurveyPara = kzalloc(sizeof(struct sitesurvey_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (psurveyPara == NULL) {
		rtw_mfree((unsigned char*) ph2c, sizeof(struct cmd_obj));
		return _FAIL;
	}

	rtw_free_network_queue(padapter, _FALSE);

	RT_TRACE(_module_rtl871x_cmd_c_, _drv_info_, ("\nflush  network queue\n\n"));

	init_h2fwcmd_w_parm_no_rsp(ph2c, psurveyPara, GEN_CMD_CODE(_SiteSurvey));

	/* psurveyPara->bsslimit = 48; */
	psurveyPara->scan_mode = pmlmepriv->scan_mode;

	/* prepare ssid list */
	if (ssid) {
		int i;
		for (i=0; i<ssid_num && i< RTW_SSID_SCAN_AMOUNT; i++) {
			if (ssid[i].SsidLength) {
				memcpy(&psurveyPara->ssid[i], &ssid[i], sizeof(NDIS_802_11_SSID));
				psurveyPara->ssid_num++;
				if (0)
				DBG_871X(FUNC_ADPT_FMT" ssid:(%s, %d)\n", FUNC_ADPT_ARG(padapter),
					psurveyPara->ssid[i].Ssid, psurveyPara->ssid[i].SsidLength);
			}
		}
	}

	/* prepare channel list */
	if (ch) {
		int i;
		for (i=0; i<ch_num && i< RTW_CHANNEL_SCAN_AMOUNT; i++) {
			if (ch[i].hw_value && !(ch[i].flags & RTW_IEEE80211_CHAN_DISABLED)) {
				memcpy(&psurveyPara->ch[i], &ch[i], sizeof(struct rtw_ieee80211_channel));
				psurveyPara->ch_num++;
				if (0)
				DBG_871X(FUNC_ADPT_FMT" ch:%u\n", FUNC_ADPT_ARG(padapter),
					psurveyPara->ch[i].hw_value);
			}
		}
	}

	set_fwstate(pmlmepriv, _FW_UNDER_SURVEY);

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);

	if(res == _SUCCESS) {

		pmlmepriv->scan_start_time = rtw_get_current_time();

#ifdef CONFIG_STA_MODE_SCAN_UNDER_AP_MODE
		if (padapter->pbuddy_adapter == NULL )
			goto full_scan_timeout;
		if((padapter->pbuddy_adapter->mlmeextpriv.mlmext_info.state&0x03) == WIFI_FW_AP_STATE)
			_set_timer(&pmlmepriv->scan_to_timer,
				SURVEY_TO * ( padapter->mlmeextpriv.max_chan_nums + ( padapter->mlmeextpriv.max_chan_nums  / RTW_SCAN_NUM_OF_CH ) * RTW_STAY_AP_CH_MILLISECOND ) + 1000 );
		else
#endif //CONFIG_STA_MODE_SCAN_UNDER_AP_MODE
full_scan_timeout:
			_set_timer(&pmlmepriv->scan_to_timer, SCANNING_TIMEOUT);

		rtw_led_control(padapter, LED_CTL_SITE_SURVEY);

	} else {
		_clr_fwstate_(pmlmepriv, _FW_UNDER_SURVEY);
	}

_func_exit_;

	return res;
}

u8 rtw_setdatarate_cmd(_adapter *padapter, u8 *rateset)
{
	struct cmd_obj*			ph2c;
	struct setdatarate_parm*	pbsetdataratepara;
	struct cmd_priv*		pcmdpriv = &padapter->cmdpriv;
	u8	res = _SUCCESS;

_func_enter_;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (ph2c == NULL) {
		res = _FAIL;
		goto exit;
	}

	pbsetdataratepara = kzalloc(sizeof(struct setdatarate_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (pbsetdataratepara == NULL) {
		rtw_mfree((u8 *) ph2c, sizeof(struct cmd_obj));
		res = _FAIL;
		goto exit;
	}

	init_h2fwcmd_w_parm_no_rsp(ph2c, pbsetdataratepara, GEN_CMD_CODE(_SetDataRate));
#ifdef MP_FIRMWARE_OFFLOAD
	pbsetdataratepara->curr_rateidx = *(u32*)rateset;
//	memcpy(pbsetdataratepara, rateset, sizeof(u32));
#else
	pbsetdataratepara->mac_id = 5;
	memcpy(pbsetdataratepara->datarates, rateset, NumRates);
#endif
	res = rtw_enqueue_cmd(pcmdpriv, ph2c);
exit:

_func_exit_;

	return res;
}

u8 rtw_setbasicrate_cmd(_adapter *padapter, u8 *rateset)
{
	struct cmd_obj*			ph2c;
	struct setbasicrate_parm*	pssetbasicratepara;
	struct cmd_priv*		pcmdpriv=&padapter->cmdpriv;
	u8	res = _SUCCESS;

_func_enter_;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (ph2c == NULL) {
		res= _FAIL;
		goto exit;
	}
	pssetbasicratepara = kzalloc(sizeof(struct setbasicrate_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);

	if (pssetbasicratepara == NULL) {
		rtw_mfree((u8*) ph2c, sizeof(struct cmd_obj));
		res = _FAIL;
		goto exit;
	}

	init_h2fwcmd_w_parm_no_rsp(ph2c, pssetbasicratepara, _SetBasicRate_CMD_);

	memcpy(pssetbasicratepara->basicrates, rateset, NumRates);

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);
exit:

_func_exit_;

	return res;
}


/*
unsigned char rtw_setphy_cmd(unsigned char  *adapter)

1.  be called only after rtw_update_registrypriv_dev_network( ~) or mp testing program
2.  for AdHoc/Ap mode or mp mode?

*/
u8 rtw_setphy_cmd(_adapter *padapter, u8 modem, u8 ch)
{
	struct cmd_obj*			ph2c;
	struct setphy_parm*		psetphypara;
	struct cmd_priv				*pcmdpriv=&padapter->cmdpriv;
//	struct mlme_priv			*pmlmepriv = &padapter->mlmepriv;
//	struct registry_priv*		pregistry_priv = &padapter->registrypriv;
	u8	res=_SUCCESS;

_func_enter_;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
		}
	psetphypara = kzalloc(sizeof(struct setphy_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);

	if(psetphypara==NULL){
		rtw_mfree((u8 *) ph2c, sizeof(struct	cmd_obj));
		res= _FAIL;
		goto exit;
	}

	init_h2fwcmd_w_parm_no_rsp(ph2c, psetphypara, _SetPhy_CMD_);

	RT_TRACE(_module_rtl871x_cmd_c_,_drv_info_,("CH=%d, modem=%d", ch, modem));

	psetphypara->modem = modem;
	psetphypara->rfchannel = ch;

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);
exit:
_func_exit_;
	return res;
}

u8 rtw_setbbreg_cmd(_adapter*padapter, u8 offset, u8 val)
{
	struct cmd_obj*			ph2c;
	struct writeBB_parm*		pwritebbparm;
	struct cmd_priv				*pcmdpriv=&padapter->cmdpriv;
	u8	res=_SUCCESS;
_func_enter_;
	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
		}
	pwritebbparm = kzalloc(sizeof(struct writeBB_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);

	if(pwritebbparm==NULL){
		rtw_mfree((u8 *) ph2c, sizeof(struct	cmd_obj));
		res= _FAIL;
		goto exit;
	}

	init_h2fwcmd_w_parm_no_rsp(ph2c, pwritebbparm, GEN_CMD_CODE(_SetBBReg));

	pwritebbparm->offset = offset;
	pwritebbparm->value = val;

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);
exit:
_func_exit_;
	return res;
}

u8 rtw_getbbreg_cmd(_adapter  *padapter, u8 offset, u8 *pval)
{
	struct cmd_obj*			ph2c;
	struct readBB_parm*		prdbbparm;
	struct cmd_priv				*pcmdpriv=&padapter->cmdpriv;
	u8	res=_SUCCESS;

_func_enter_;
	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res=_FAIL;
		goto exit;
		}
	prdbbparm = kzalloc(sizeof(struct readBB_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);

	if(prdbbparm ==NULL){
		rtw_mfree((unsigned char *) ph2c, sizeof(struct	cmd_obj));
		return _FAIL;
	}

	_rtw_init_listhead(&ph2c->list);
	ph2c->cmdcode =GEN_CMD_CODE(_GetBBReg);
	ph2c->parmbuf = (unsigned char *)prdbbparm;
	ph2c->cmdsz =  sizeof(struct readBB_parm);
	ph2c->rsp = pval;
	ph2c->rspsz = sizeof(struct readBB_rsp);

	prdbbparm ->offset = offset;

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);
exit:
_func_exit_;
	return res;
}

u8 rtw_setrfreg_cmd(_adapter  *padapter, u8 offset, u32 val)
{
	struct cmd_obj*			ph2c;
	struct writeRF_parm*		pwriterfparm;
	struct cmd_priv				*pcmdpriv=&padapter->cmdpriv;
	u8	res=_SUCCESS;
_func_enter_;
	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
	}
	pwriterfparm = kzalloc(sizeof(struct writeRF_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);

	if(pwriterfparm==NULL){
		rtw_mfree((u8 *) ph2c, sizeof(struct	cmd_obj));
		res= _FAIL;
		goto exit;
	}

	init_h2fwcmd_w_parm_no_rsp(ph2c, pwriterfparm, GEN_CMD_CODE(_SetRFReg));

	pwriterfparm->offset = offset;
	pwriterfparm->value = val;

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);
exit:
_func_exit_;
	return res;
}

u8 rtw_getrfreg_cmd(_adapter  *padapter, u8 offset, u8 *pval)
{
	struct cmd_obj*			ph2c;
	struct readRF_parm*		prdrfparm;
	struct cmd_priv				*pcmdpriv=&padapter->cmdpriv;
	u8	res=_SUCCESS;

_func_enter_;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
	}

	prdrfparm = kzalloc(sizeof(struct readRF_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(prdrfparm ==NULL){
		rtw_mfree((u8 *) ph2c, sizeof(struct	cmd_obj));
		res= _FAIL;
		goto exit;
	}

	_rtw_init_listhead(&ph2c->list);
	ph2c->cmdcode =GEN_CMD_CODE(_GetRFReg);
	ph2c->parmbuf = (unsigned char *)prdrfparm;
	ph2c->cmdsz =  sizeof(struct readRF_parm);
	ph2c->rsp = pval;
	ph2c->rspsz = sizeof(struct readRF_rsp);

	prdrfparm ->offset = offset;

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);

exit:

_func_exit_;

	return res;
}

void rtw_getbbrfreg_cmdrsp_callback(_adapter*	padapter,  struct cmd_obj *pcmd)
{
 _func_enter_;

	//rtw_free_cmd_obj(pcmd);
	rtw_mfree((unsigned char*) pcmd->parmbuf, pcmd->cmdsz);
	rtw_mfree((unsigned char*) pcmd, sizeof(struct cmd_obj));

#ifdef CONFIG_MP_INCLUDED
	padapter->mppriv.workparam.bcompleted= _TRUE;
#endif
_func_exit_;
}

void rtw_readtssi_cmdrsp_callback(_adapter*	padapter,  struct cmd_obj *pcmd)
{
 _func_enter_;

	rtw_mfree((unsigned char*) pcmd->parmbuf, pcmd->cmdsz);
	rtw_mfree((unsigned char*) pcmd, sizeof(struct cmd_obj));

#ifdef CONFIG_MP_INCLUDED
	padapter->mppriv.workparam.bcompleted= _TRUE;
#endif

_func_exit_;
}

u8 rtw_createbss_cmd(_adapter  *padapter)
{
	struct cmd_obj*			pcmd;
	struct cmd_priv				*pcmdpriv=&padapter->cmdpriv;
	struct mlme_priv			*pmlmepriv = &padapter->mlmepriv;
	WLAN_BSSID_EX		*pdev_network = &padapter->registrypriv.dev_network;
	u8	res=_SUCCESS;

_func_enter_;

	rtw_led_control(padapter, LED_CTL_START_TO_LINK);

	if (pmlmepriv->assoc_ssid.SsidLength == 0){
		RT_TRACE(_module_rtl871x_cmd_c_,_drv_info_,(" createbss for Any SSid:%s\n",pmlmepriv->assoc_ssid.Ssid));
	} else {
		RT_TRACE(_module_rtl871x_cmd_c_,_drv_info_,(" createbss for SSid:%s\n", pmlmepriv->assoc_ssid.Ssid));
	}

	pcmd = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(pcmd==NULL){
		res= _FAIL;
		goto exit;
	}

	_rtw_init_listhead(&pcmd->list);
	pcmd->cmdcode = _CreateBss_CMD_;
	pcmd->parmbuf = (unsigned char *)pdev_network;
	pcmd->cmdsz = get_WLAN_BSSID_EX_sz((WLAN_BSSID_EX*)pdev_network);
	pcmd->rsp = NULL;
	pcmd->rspsz = 0;

	pdev_network->Length = pcmd->cmdsz;

	res = rtw_enqueue_cmd(pcmdpriv, pcmd);

exit:

_func_exit_;

	return res;
}

u8 rtw_createbss_cmd_ex(_adapter  *padapter, unsigned char *pbss, unsigned int sz)
{
	struct cmd_obj*	pcmd;
	struct cmd_priv		*pcmdpriv=&padapter->cmdpriv;
	u8	res=_SUCCESS;

_func_enter_;

	pcmd = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(pcmd==NULL){
		res= _FAIL;
		goto exit;
	}

	_rtw_init_listhead(&pcmd->list);
	pcmd->cmdcode = GEN_CMD_CODE(_CreateBss);
	pcmd->parmbuf = pbss;
	pcmd->cmdsz =  sz;
	pcmd->rsp = NULL;
	pcmd->rspsz = 0;

	res = rtw_enqueue_cmd(pcmdpriv, pcmd);

exit:

_func_exit_;

	return res;
}

u8 rtw_joinbss_cmd(_adapter  *padapter, struct wlan_network* pnetwork)
{
	u8	*auth, res = _SUCCESS;
	uint	t_len = 0;
	WLAN_BSSID_EX		*psecnetwork;
	struct cmd_obj		*pcmd;
	struct cmd_priv		*pcmdpriv=&padapter->cmdpriv;
	struct mlme_priv		*pmlmepriv = &padapter->mlmepriv;
	struct qos_priv		*pqospriv= &pmlmepriv->qospriv;
	struct security_priv	*psecuritypriv=&padapter->securitypriv;
	struct registry_priv	*pregistrypriv = &padapter->registrypriv;
	struct ht_priv			*phtpriv = &pmlmepriv->htpriv;
	NDIS_802_11_NETWORK_INFRASTRUCTURE ndis_network_mode = pnetwork->network.InfrastructureMode;
	struct mlme_ext_priv	*pmlmeext = &padapter->mlmeextpriv;
	struct mlme_ext_info	*pmlmeinfo = &(pmlmeext->mlmext_info);

_func_enter_;

	rtw_led_control(padapter, LED_CTL_START_TO_LINK);

	if (pmlmepriv->assoc_ssid.SsidLength == 0){
		RT_TRACE(_module_rtl871x_cmd_c_, _drv_info_, ("+Join cmd: Any SSid\n"));
	} else {
		RT_TRACE(_module_rtl871x_cmd_c_, _drv_notice_, ("+Join cmd: SSid=[%s]\n", pmlmepriv->assoc_ssid.Ssid));
	}

	pcmd = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(pcmd==NULL){
		res=_FAIL;
		RT_TRACE(_module_rtl871x_cmd_c_, _drv_err_, ("rtw_joinbss_cmd: memory allocate for cmd_obj fail!!!\n"));
		goto exit;
	}
	/* // for IEs is pointer
	t_len = sizeof (ULONG) + sizeof (NDIS_802_11_MAC_ADDRESS) + 2 +
			sizeof (NDIS_802_11_SSID) + sizeof (ULONG) +
			sizeof (NDIS_802_11_RSSI) + sizeof (NDIS_802_11_NETWORK_TYPE) +
			sizeof (NDIS_802_11_CONFIGURATION) +
			sizeof (NDIS_802_11_NETWORK_INFRASTRUCTURE) +
			sizeof (NDIS_802_11_RATES_EX)+ sizeof(WLAN_PHY_INFO)+ sizeof (ULONG) + MAX_IE_SZ;
	*/
	//for IEs is fix buf size
	t_len = sizeof(WLAN_BSSID_EX);


	//for hidden ap to set fw_state here
	if (check_fwstate(pmlmepriv, WIFI_STATION_STATE|WIFI_ADHOC_STATE) != _TRUE)
	{
		switch(ndis_network_mode)
		{
			case Ndis802_11IBSS:
				set_fwstate(pmlmepriv, WIFI_ADHOC_STATE);
				break;

			case Ndis802_11Infrastructure:
				set_fwstate(pmlmepriv, WIFI_STATION_STATE);
				break;

			case Ndis802_11APMode:
			case Ndis802_11AutoUnknown:
			case Ndis802_11InfrastructureMax:
				break;

		}
	}

	psecnetwork=(WLAN_BSSID_EX *)&psecuritypriv->sec_bss;
	if(psecnetwork==NULL)
	{
		if(pcmd !=NULL)
			rtw_mfree((unsigned char *)pcmd, sizeof(struct	cmd_obj));

		res=_FAIL;

		RT_TRACE(_module_rtl871x_cmd_c_, _drv_err_, ("rtw_joinbss_cmd :psecnetwork==NULL!!!\n"));

		goto exit;
	}

	_rtw_memset(psecnetwork, 0, t_len);

	memcpy(psecnetwork, &pnetwork->network, get_WLAN_BSSID_EX_sz(&pnetwork->network));

	auth=&psecuritypriv->authenticator_ie[0];
	psecuritypriv->authenticator_ie[0]=(unsigned char)psecnetwork->IELength;

	if((psecnetwork->IELength-12) < (256-1)) {
		memcpy(&psecuritypriv->authenticator_ie[1], &psecnetwork->IEs[12], psecnetwork->IELength-12);
	} else {
		memcpy(&psecuritypriv->authenticator_ie[1], &psecnetwork->IEs[12], (256-1));
	}

	psecnetwork->IELength = 0;
	// Added by Albert 2009/02/18
	// If the the driver wants to use the bssid to create the connection.
	// If not,  we have to copy the connecting AP's MAC address to it so that
	// the driver just has the bssid information for PMKIDList searching.

	if ( pmlmepriv->assoc_by_bssid == _FALSE )
	{
		memcpy( &pmlmepriv->assoc_bssid[ 0 ], &pnetwork->network.MacAddress[ 0 ], ETH_ALEN );
	}

	psecnetwork->IELength = rtw_restruct_sec_ie(padapter, &pnetwork->network.IEs[0], &psecnetwork->IEs[0], pnetwork->network.IELength);


	pqospriv->qos_option = 0;

	if(pregistrypriv->wmm_enable)
	{
		u32 tmp_len;

		tmp_len = rtw_restruct_wmm_ie(padapter, &pnetwork->network.IEs[0], &psecnetwork->IEs[0], pnetwork->network.IELength, psecnetwork->IELength);

		if (psecnetwork->IELength != tmp_len)
		{
			psecnetwork->IELength = tmp_len;
			pqospriv->qos_option = 1; //There is WMM IE in this corresp. beacon
		}
		else
		{
			pqospriv->qos_option = 0;//There is no WMM IE in this corresp. beacon
		}
	}

#ifdef CONFIG_80211N_HT
	phtpriv->ht_option = _FALSE;
	if(pregistrypriv->ht_enable)
	{
		//	Added by Albert 2010/06/23
		//	For the WEP mode, we will use the bg mode to do the connection to avoid some IOT issue.
		//	Especially for Realtek 8192u SoftAP.
		if (	( padapter->securitypriv.dot11PrivacyAlgrthm != _WEP40_ ) &&
			( padapter->securitypriv.dot11PrivacyAlgrthm != _WEP104_ ) &&
			( padapter->securitypriv.dot11PrivacyAlgrthm != _TKIP_ ))
		{
			//rtw_restructure_ht_ie
			rtw_restructure_ht_ie(padapter, &pnetwork->network.IEs[0], &psecnetwork->IEs[0],
									pnetwork->network.IELength, &psecnetwork->IELength, (u8)psecnetwork->Configuration.DSConfig );
		}
	}

#endif

	pmlmeinfo->assoc_AP_vendor = check_assoc_AP(pnetwork->network.IEs, pnetwork->network.IELength);

	#if 0
	psecuritypriv->supplicant_ie[0]=(u8)psecnetwork->IELength;

	if(psecnetwork->IELength < (256-1))
	{
		memcpy(&psecuritypriv->supplicant_ie[1], &psecnetwork->IEs[0], psecnetwork->IELength);
	}
	else
	{
		memcpy(&psecuritypriv->supplicant_ie[1], &psecnetwork->IEs[0], (256-1));
	}
	#endif

	pcmd->cmdsz = get_WLAN_BSSID_EX_sz(psecnetwork);//get cmdsz before endian conversion

	_rtw_init_listhead(&pcmd->list);
	pcmd->cmdcode = _JoinBss_CMD_;//GEN_CMD_CODE(_JoinBss)
	pcmd->parmbuf = (unsigned char *)psecnetwork;
	pcmd->rsp = NULL;
	pcmd->rspsz = 0;

	res = rtw_enqueue_cmd(pcmdpriv, pcmd);

exit:

_func_exit_;

	return res;
}

u8 rtw_disassoc_cmd(_adapter*padapter, u32 deauth_timeout_ms, bool enqueue) /* for sta_mode */
{
	struct cmd_obj *cmdobj = NULL;
	struct disconnect_parm *param = NULL;
	struct cmd_priv *cmdpriv = &padapter->cmdpriv;
	u8 res = _SUCCESS;

_func_enter_;

	RT_TRACE(_module_rtl871x_cmd_c_, _drv_notice_, ("+rtw_disassoc_cmd\n"));

	/* prepare cmd parameter */
	param = kzalloc(sizeof(*param), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (param == NULL) {
		res = _FAIL;
		goto exit;
	}
	param->deauth_timeout_ms = deauth_timeout_ms;

	if (enqueue) {
		/* need enqueue, prepare cmd_obj and enqueue */
		cmdobj = kzalloc(sizeof(*cmdobj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if (cmdobj == NULL) {
			res = _FAIL;
			rtw_mfree((u8 *)param, sizeof(*param));
			goto exit;
		}
		init_h2fwcmd_w_parm_no_rsp(cmdobj, param, _DisConnect_CMD_);
		res = rtw_enqueue_cmd(cmdpriv, cmdobj);
	} else {
		/* no need to enqueue, do the cmd hdl directly and free cmd parameter */
		if (H2C_SUCCESS != disconnect_hdl(padapter, (u8 *)param))
			res = _FAIL;
		rtw_mfree((u8 *)param, sizeof(*param));
	}

exit:

_func_exit_;

	return res;
}

u8 rtw_setopmode_cmd(_adapter  *padapter, NDIS_802_11_NETWORK_INFRASTRUCTURE networktype, bool enqueue)
{
	struct	cmd_obj*	ph2c;
	struct	setopmode_parm* psetop;

	struct	cmd_priv   *pcmdpriv= &padapter->cmdpriv;
	u8	res=_SUCCESS;

_func_enter_;

	psetop = kzalloc(sizeof(struct setopmode_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(psetop==NULL){
		res=_FAIL;
		goto exit;
	}
	psetop->mode = (u8)networktype;

	if (enqueue) {
		ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if(ph2c==NULL){
			rtw_mfree((u8 *)psetop, sizeof(*psetop));
			res= _FAIL;
			goto exit;
		}

		init_h2fwcmd_w_parm_no_rsp(ph2c, psetop, _SetOpMode_CMD_);
		res = rtw_enqueue_cmd(pcmdpriv, ph2c);
	}
	else{
		setopmode_hdl(padapter, (u8 *)psetop);
		rtw_mfree((u8 *)psetop, sizeof(*psetop));
	}

exit:

_func_exit_;

	return res;
}

u8 rtw_setstakey_cmd(_adapter *padapter, struct sta_info *sta, u8 unicast_key, bool enqueue)
{
	struct cmd_obj*			ph2c;
	struct set_stakey_parm	*psetstakey_para;
	struct cmd_priv				*pcmdpriv=&padapter->cmdpriv;
	struct set_stakey_rsp		*psetstakey_rsp = NULL;

	struct mlme_priv			*pmlmepriv = &padapter->mlmepriv;
	struct security_priv		*psecuritypriv = &padapter->securitypriv;
	u8	res=_SUCCESS;

_func_enter_;

	psetstakey_para = kzalloc(sizeof(struct set_stakey_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(psetstakey_para==NULL){
		res=_FAIL;
		goto exit;
	}

	memcpy(psetstakey_para->addr, sta->hwaddr,ETH_ALEN);

	if(check_fwstate(pmlmepriv, WIFI_STATION_STATE)){
		#ifdef CONFIG_TDLS
		if(sta->tdls_sta_state&TDLS_LINKED_STATE)
			psetstakey_para->algorithm=(u8)sta->dot118021XPrivacy;
		else
		#endif //CONFIG_TDLS
			psetstakey_para->algorithm =(unsigned char) psecuritypriv->dot11PrivacyAlgrthm;
	}else{
		GET_ENCRY_ALGO(psecuritypriv, sta, psetstakey_para->algorithm, _FALSE);
	}

	if (unicast_key == _TRUE) {
		#ifdef CONFIG_TDLS
		if(sta->tdls_sta_state&TDLS_LINKED_STATE)
			memcpy(&psetstakey_para->key, sta->tpk.tk, 16);
		else
		#endif //CONFIG_TDLS
			memcpy(&psetstakey_para->key, &sta->dot118021x_UncstKey, 16);
	}
	else {
		memcpy(&psetstakey_para->key, &psecuritypriv->dot118021XGrpKey[psecuritypriv->dot118021XGrpKeyid].skey, 16);
	}

	//jeff: set this becasue at least sw key is ready
	padapter->securitypriv.busetkipkey=_TRUE;

	if (enqueue) {
		ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if ( ph2c == NULL){
			rtw_mfree((u8 *) psetstakey_para, sizeof(struct set_stakey_parm));
			res= _FAIL;
			goto exit;
		}

		psetstakey_rsp = kzalloc(sizeof(struct set_stakey_rsp), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if(psetstakey_rsp == NULL){
			rtw_mfree((u8 *) ph2c, sizeof(struct cmd_obj));
			rtw_mfree((u8 *) psetstakey_para, sizeof(struct set_stakey_parm));
			res=_FAIL;
			goto exit;
		}

		init_h2fwcmd_w_parm_no_rsp(ph2c, psetstakey_para, _SetStaKey_CMD_);
		ph2c->rsp = (u8 *) psetstakey_rsp;
		ph2c->rspsz = sizeof(struct set_stakey_rsp);
		res = rtw_enqueue_cmd(pcmdpriv, ph2c);
	}
	else {
		set_stakey_hdl(padapter, (u8 *)psetstakey_para);
		rtw_mfree((u8 *) psetstakey_para, sizeof(struct set_stakey_parm));
	}

exit:

_func_exit_;

	return res;
}

u8 rtw_clearstakey_cmd(_adapter *padapter, struct sta_info *sta, u8 enqueue)
{
	struct cmd_obj*			ph2c;
	struct set_stakey_parm	*psetstakey_para;
	struct cmd_priv				*pcmdpriv=&padapter->cmdpriv;
	struct set_stakey_rsp		*psetstakey_rsp = NULL;
	struct mlme_priv			*pmlmepriv = &padapter->mlmepriv;
	struct security_priv		*psecuritypriv = &padapter->securitypriv;
	s16 cam_id = 0;
	u8	res=_SUCCESS;

_func_enter_;

	if(!enqueue)
	{
		while((cam_id = rtw_camid_search(padapter, sta->hwaddr, -1)) >= 0) {
			DBG_871X_LEVEL(_drv_always_, "clear key for addr:"MAC_FMT", camid:%d\n", MAC_ARG(sta->hwaddr), cam_id);
			clear_cam_entry(padapter, cam_id);
			rtw_camid_free(padapter, cam_id);
		}
	} else {
		ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if ( ph2c == NULL){
			res= _FAIL;
			goto exit;
		}

		psetstakey_para = kzalloc(sizeof(struct set_stakey_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if(psetstakey_para==NULL){
			rtw_mfree((u8 *) ph2c, sizeof(struct	cmd_obj));
			res=_FAIL;
			goto exit;
		}

		psetstakey_rsp = kzalloc(sizeof(struct set_stakey_rsp), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if(psetstakey_rsp == NULL){
			rtw_mfree((u8 *) ph2c, sizeof(struct	cmd_obj));
			rtw_mfree((u8 *) psetstakey_para, sizeof(struct set_stakey_parm));
			res=_FAIL;
			goto exit;
		}

		init_h2fwcmd_w_parm_no_rsp(ph2c, psetstakey_para, _SetStaKey_CMD_);
		ph2c->rsp = (u8 *) psetstakey_rsp;
		ph2c->rspsz = sizeof(struct set_stakey_rsp);

		memcpy(psetstakey_para->addr, sta->hwaddr, ETH_ALEN);

		psetstakey_para->algorithm = _NO_PRIVACY_;

		res = rtw_enqueue_cmd(pcmdpriv, ph2c);

	}

exit:

_func_exit_;

	return res;
}

u8 rtw_setrttbl_cmd(_adapter  *padapter, struct setratable_parm *prate_table)
{
	struct cmd_obj*			ph2c;
	struct setratable_parm *	psetrttblparm;
	struct cmd_priv				*pcmdpriv=&padapter->cmdpriv;
	u8	res=_SUCCESS;
_func_enter_;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
		}
	psetrttblparm = kzalloc(sizeof(struct setratable_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);

	if(psetrttblparm==NULL){
		rtw_mfree((unsigned char *) ph2c, sizeof(struct	cmd_obj));
		res= _FAIL;
		goto exit;
	}

	init_h2fwcmd_w_parm_no_rsp(ph2c, psetrttblparm, GEN_CMD_CODE(_SetRaTable));

	memcpy(psetrttblparm,prate_table,sizeof(struct setratable_parm));

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);
exit:
_func_exit_;
	return res;

}

u8 rtw_getrttbl_cmd(_adapter  *padapter, struct getratable_rsp *pval)
{
	struct cmd_obj*			ph2c;
	struct getratable_parm *	pgetrttblparm;
	struct cmd_priv				*pcmdpriv=&padapter->cmdpriv;
	u8	res=_SUCCESS;
_func_enter_;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
	}
	pgetrttblparm = kzalloc(sizeof(struct getratable_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);

	if(pgetrttblparm==NULL){
		rtw_mfree((unsigned char *) ph2c, sizeof(struct	cmd_obj));
		res= _FAIL;
		goto exit;
	}

//	init_h2fwcmd_w_parm_no_rsp(ph2c, psetrttblparm, GEN_CMD_CODE(_SetRaTable));

	_rtw_init_listhead(&ph2c->list);
	ph2c->cmdcode =GEN_CMD_CODE(_GetRaTable);
	ph2c->parmbuf = (unsigned char *)pgetrttblparm;
	ph2c->cmdsz =  sizeof(struct getratable_parm);
	ph2c->rsp = (u8*)pval;
	ph2c->rspsz = sizeof(struct getratable_rsp);

	pgetrttblparm ->rsvd = 0x0;

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);
exit:
_func_exit_;
	return res;

}

u8 rtw_setassocsta_cmd(_adapter  *padapter, u8 *mac_addr)
{
	struct cmd_priv			*pcmdpriv = &padapter->cmdpriv;
	struct cmd_obj*			ph2c;
	struct set_assocsta_parm	*psetassocsta_para;
	struct set_stakey_rsp		*psetassocsta_rsp = NULL;

	u8	res=_SUCCESS;

_func_enter_;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
	}

	psetassocsta_para = kzalloc(sizeof(struct set_assocsta_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(psetassocsta_para==NULL){
		rtw_mfree((u8 *) ph2c, sizeof(struct	cmd_obj));
		res=_FAIL;
		goto exit;
	}

	psetassocsta_rsp = kzalloc(sizeof(struct set_assocsta_rsp), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(psetassocsta_rsp==NULL){
		rtw_mfree((u8 *) ph2c, sizeof(struct	cmd_obj));
		rtw_mfree((u8 *) psetassocsta_para, sizeof(struct set_assocsta_parm));
		return _FAIL;
	}

	init_h2fwcmd_w_parm_no_rsp(ph2c, psetassocsta_para, _SetAssocSta_CMD_);
	ph2c->rsp = (u8 *) psetassocsta_rsp;
	ph2c->rspsz = sizeof(struct set_assocsta_rsp);

	memcpy(psetassocsta_para->addr, mac_addr,ETH_ALEN);

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);

exit:

_func_exit_;

	return res;
 }

u8 rtw_addbareq_cmd(_adapter*padapter, u8 tid, u8 *addr)
{
	struct cmd_priv		*pcmdpriv = &padapter->cmdpriv;
	struct cmd_obj*		ph2c;
	struct addBaReq_parm	*paddbareq_parm;

	u8	res=_SUCCESS;

_func_enter_;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
	}

	paddbareq_parm = kzalloc(sizeof(struct addBaReq_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(paddbareq_parm==NULL){
		rtw_mfree((unsigned char *)ph2c, sizeof(struct	cmd_obj));
		res= _FAIL;
		goto exit;
	}

	paddbareq_parm->tid = tid;
	memcpy(paddbareq_parm->addr, addr, ETH_ALEN);

	init_h2fwcmd_w_parm_no_rsp(ph2c, paddbareq_parm, GEN_CMD_CODE(_AddBAReq));

	//DBG_871X("rtw_addbareq_cmd, tid=%d\n", tid);

	//rtw_enqueue_cmd(pcmdpriv, ph2c);
	res = rtw_enqueue_cmd(pcmdpriv, ph2c);

exit:

_func_exit_;

	return res;
}
//add for CONFIG_IEEE80211W, none 11w can use it
u8 rtw_reset_securitypriv_cmd(_adapter*padapter)
{
	struct cmd_obj*		ph2c;
	struct drvextra_cmd_parm  *pdrvextra_cmd_parm;
	struct cmd_priv	*pcmdpriv=&padapter->cmdpriv;
	u8	res=_SUCCESS;

_func_enter_;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm = kzalloc(sizeof(struct drvextra_cmd_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(pdrvextra_cmd_parm==NULL){
		rtw_mfree((unsigned char *)ph2c, sizeof(struct cmd_obj));
		res= _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm->ec_id = RESET_SECURITYPRIV;
	pdrvextra_cmd_parm->type_size = 0;
	pdrvextra_cmd_parm->pbuf = (u8 *)padapter;

	init_h2fwcmd_w_parm_no_rsp(ph2c, pdrvextra_cmd_parm, GEN_CMD_CODE(_Set_Drv_Extra));


	//rtw_enqueue_cmd(pcmdpriv, ph2c);
	res = rtw_enqueue_cmd(pcmdpriv, ph2c);

exit:

_func_exit_;

	return res;

}

u8 rtw_free_assoc_resources_cmd(_adapter*padapter)
{
	struct cmd_obj*		ph2c;
	struct drvextra_cmd_parm  *pdrvextra_cmd_parm;
	struct cmd_priv	*pcmdpriv=&padapter->cmdpriv;
	u8	res=_SUCCESS;

_func_enter_;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm = kzalloc(sizeof(struct drvextra_cmd_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(pdrvextra_cmd_parm==NULL){
		rtw_mfree((unsigned char *)ph2c, sizeof(struct cmd_obj));
		res= _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm->ec_id = FREE_ASSOC_RESOURCES;
	pdrvextra_cmd_parm->type_size = 0;
	pdrvextra_cmd_parm->pbuf = (u8 *)padapter;

	init_h2fwcmd_w_parm_no_rsp(ph2c, pdrvextra_cmd_parm, GEN_CMD_CODE(_Set_Drv_Extra));


	//rtw_enqueue_cmd(pcmdpriv, ph2c);
	res = rtw_enqueue_cmd(pcmdpriv, ph2c);

exit:

_func_exit_;

	return res;

}


u8 rtw_dynamic_chk_wk_cmd(_adapter*padapter)
{
	struct cmd_obj*		ph2c;
	struct drvextra_cmd_parm  *pdrvextra_cmd_parm;
	struct cmd_priv	*pcmdpriv=&padapter->cmdpriv;
	u8	res=_SUCCESS;

_func_enter_;

	if ((padapter->bDriverStopped == _TRUE)||(padapter->bSurpriseRemoved== _TRUE))
		goto exit;


#ifdef CONFIG_CONCURRENT_MODE
	if (padapter->adapter_type != PRIMARY_ADAPTER && padapter->pbuddy_adapter)
		pcmdpriv = &(padapter->pbuddy_adapter->cmdpriv);
#endif

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm = kzalloc(sizeof(struct drvextra_cmd_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(pdrvextra_cmd_parm==NULL){
		rtw_mfree((unsigned char *)ph2c, sizeof(struct cmd_obj));
		res= _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm->ec_id = DYNAMIC_CHK_WK_CID;
	pdrvextra_cmd_parm->type_size = 0;
	pdrvextra_cmd_parm->pbuf = (u8 *)padapter;

	init_h2fwcmd_w_parm_no_rsp(ph2c, pdrvextra_cmd_parm, GEN_CMD_CODE(_Set_Drv_Extra));


	//rtw_enqueue_cmd(pcmdpriv, ph2c);
	res = rtw_enqueue_cmd(pcmdpriv, ph2c);

exit:

_func_exit_;

	return res;

}

u8 rtw_set_ch_cmd(_adapter*padapter, u8 ch, u8 bw, u8 ch_offset, u8 enqueue)
{
	struct cmd_obj *pcmdobj;
	struct set_ch_parm *set_ch_parm;
	struct cmd_priv *pcmdpriv = &padapter->cmdpriv;

	u8 res=_SUCCESS;

_func_enter_;

	DBG_871X(FUNC_NDEV_FMT" ch:%u, bw:%u, ch_offset:%u\n",
		FUNC_NDEV_ARG(padapter->pnetdev), ch, bw, ch_offset);

	/* check input parameter */

	/* prepare cmd parameter */
	set_ch_parm = kzalloc(sizeof(*set_ch_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (set_ch_parm == NULL) {
		res= _FAIL;
		goto exit;
	}
	set_ch_parm->ch = ch;
	set_ch_parm->bw = bw;
	set_ch_parm->ch_offset = ch_offset;

	if (enqueue) {
		/* need enqueue, prepare cmd_obj and enqueue */
		pcmdobj = kzalloc(sizeof(struct	cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if(pcmdobj == NULL){
			rtw_mfree((u8 *)set_ch_parm, sizeof(*set_ch_parm));
			res=_FAIL;
			goto exit;
		}

		init_h2fwcmd_w_parm_no_rsp(pcmdobj, set_ch_parm, GEN_CMD_CODE(_SetChannel));
		res = rtw_enqueue_cmd(pcmdpriv, pcmdobj);
	} else {
		/* no need to enqueue, do the cmd hdl directly and free cmd parameter */
		if( H2C_SUCCESS !=set_ch_hdl(padapter, (u8 *)set_ch_parm) )
			res = _FAIL;

		rtw_mfree((u8 *)set_ch_parm, sizeof(*set_ch_parm));
	}

	/* do something based on res... */

exit:

	DBG_871X(FUNC_NDEV_FMT" res:%u\n", FUNC_NDEV_ARG(padapter->pnetdev), res);

_func_exit_;

	return res;
}

u8 rtw_set_chplan_cmd(_adapter*padapter, u8 chplan, u8 enqueue)
{
	struct	cmd_obj*	pcmdobj;
	struct	SetChannelPlan_param *setChannelPlan_param;
	struct	cmd_priv   *pcmdpriv = &padapter->cmdpriv;

	u8	res=_SUCCESS;

_func_enter_;

	RT_TRACE(_module_rtl871x_cmd_c_, _drv_notice_, ("+rtw_set_chplan_cmd\n"));

	//check input parameter
	if(!rtw_is_channel_plan_valid(chplan)) {
		res = _FAIL;
		goto exit;
	}

	//prepare cmd parameter
	setChannelPlan_param = kzalloc(sizeof(struct SetChannelPlan_param), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(setChannelPlan_param == NULL) {
		res= _FAIL;
		goto exit;
	}
	setChannelPlan_param->channel_plan=chplan;

	if(enqueue)
	{
		//need enqueue, prepare cmd_obj and enqueue
		pcmdobj = kzalloc(sizeof(struct	cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if(pcmdobj == NULL){
			rtw_mfree((u8 *)setChannelPlan_param, sizeof(struct SetChannelPlan_param));
			res=_FAIL;
			goto exit;
		}

		init_h2fwcmd_w_parm_no_rsp(pcmdobj, setChannelPlan_param, GEN_CMD_CODE(_SetChannelPlan));
		res = rtw_enqueue_cmd(pcmdpriv, pcmdobj);
	}
	else
	{
		//no need to enqueue, do the cmd hdl directly and free cmd parameter
		if( H2C_SUCCESS !=set_chplan_hdl(padapter, (unsigned char *)setChannelPlan_param) )
			res = _FAIL;

		rtw_mfree((u8 *)setChannelPlan_param, sizeof(struct SetChannelPlan_param));
	}

	//do something based on res...
	if(res == _SUCCESS)
		padapter->mlmepriv.ChannelPlan = chplan;

exit:

_func_exit_;

	return res;
}

u8 rtw_led_blink_cmd(_adapter*padapter, PLED_871x pLed)
{
	struct	cmd_obj*	pcmdobj;
	struct	LedBlink_param *ledBlink_param;
	struct	cmd_priv   *pcmdpriv = &padapter->cmdpriv;

	u8	res=_SUCCESS;

_func_enter_;

	RT_TRACE(_module_rtl871x_cmd_c_, _drv_notice_, ("+rtw_led_blink_cmd\n"));

	pcmdobj = kzalloc(sizeof(struct	cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(pcmdobj == NULL){
		res=_FAIL;
		goto exit;
	}

	ledBlink_param = kzalloc(sizeof(struct	LedBlink_param), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ledBlink_param == NULL) {
		rtw_mfree((u8 *)pcmdobj, sizeof(struct cmd_obj));
		res= _FAIL;
		goto exit;
	}

	ledBlink_param->pLed=pLed;

	init_h2fwcmd_w_parm_no_rsp(pcmdobj, ledBlink_param, GEN_CMD_CODE(_LedBlink));
	res = rtw_enqueue_cmd(pcmdpriv, pcmdobj);

exit:

_func_exit_;

	return res;
}

u8 rtw_set_csa_cmd(_adapter*padapter, u8 new_ch_no)
{
	struct	cmd_obj*	pcmdobj;
	struct	SetChannelSwitch_param*setChannelSwitch_param;
	struct	mlme_priv *pmlmepriv = &padapter->mlmepriv;
	struct	cmd_priv   *pcmdpriv = &padapter->cmdpriv;

	u8	res=_SUCCESS;

_func_enter_;

	RT_TRACE(_module_rtl871x_cmd_c_, _drv_notice_, ("+rtw_set_csa_cmd\n"));

	pcmdobj = kzalloc(sizeof(struct	cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(pcmdobj == NULL){
		res=_FAIL;
		goto exit;
	}

	setChannelSwitch_param = kzalloc(sizeof(struct	SetChannelSwitch_param), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(setChannelSwitch_param == NULL) {
		rtw_mfree((u8 *)pcmdobj, sizeof(struct cmd_obj));
		res= _FAIL;
		goto exit;
	}

	setChannelSwitch_param->new_ch_no=new_ch_no;

	init_h2fwcmd_w_parm_no_rsp(pcmdobj, setChannelSwitch_param, GEN_CMD_CODE(_SetChannelSwitch));
	res = rtw_enqueue_cmd(pcmdpriv, pcmdobj);

exit:

_func_exit_;

	return res;
}

u8 rtw_tdls_cmd(_adapter *padapter, u8 *addr, u8 option)
{
	struct	cmd_obj*	pcmdobj;
	struct	TDLSoption_param	*TDLSoption;
	struct	mlme_priv *pmlmepriv = &padapter->mlmepriv;
	struct	cmd_priv   *pcmdpriv = &padapter->cmdpriv;

	u8	res=_SUCCESS;

_func_enter_;

#ifdef CONFIG_TDLS

	RT_TRACE(_module_rtl871x_cmd_c_, _drv_notice_, ("+rtw_set_tdls_cmd\n"));

	pcmdobj = kzalloc(sizeof(struct	cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(pcmdobj == NULL){
		res=_FAIL;
		goto exit;
	}

	TDLSoption= kzalloc(sizeof(struct TDLSoption_param), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(TDLSoption == NULL) {
		rtw_mfree((u8 *)pcmdobj, sizeof(struct cmd_obj));
		res= _FAIL;
		goto exit;
	}

	_rtw_spinlock(&(padapter->tdlsinfo.cmd_lock));
	memcpy(TDLSoption->addr, addr, 6);
	TDLSoption->option = option;
	_rtw_spinunlock(&(padapter->tdlsinfo.cmd_lock));
	init_h2fwcmd_w_parm_no_rsp(pcmdobj, TDLSoption, GEN_CMD_CODE(_TDLS));
	res = rtw_enqueue_cmd(pcmdpriv, pcmdobj);

#endif	//CONFIG_TDLS

exit:


_func_exit_;

	return res;
}

static void traffic_status_watchdog(_adapter *padapter)
{
#ifdef CONFIG_LPS
	u8	bEnterPS;
#endif
	u16	BusyThreshold = 100;
	u8	bBusyTraffic = _FALSE, bTxBusyTraffic = _FALSE, bRxBusyTraffic = _FALSE;
	u8	bHigherBusyTraffic = _FALSE, bHigherBusyRxTraffic = _FALSE, bHigherBusyTxTraffic = _FALSE;
	struct mlme_priv		*pmlmepriv = &(padapter->mlmepriv);
#ifdef CONFIG_TDLS
	struct tdls_info *ptdlsinfo = &(padapter->tdlsinfo);
#endif //CONFIG_TDLS

	RT_LINK_DETECT_T * link_detect = &pmlmepriv->LinkDetectInfo;

	//
	// Determine if our traffic is busy now
	//
	if((check_fwstate(pmlmepriv, _FW_LINKED)== _TRUE)
		/*&& !MgntInitAdapterInProgress(pMgntInfo)*/)
	{

		// if we raise bBusyTraffic in last watchdog, using lower threshold.
		if (pmlmepriv->LinkDetectInfo.bBusyTraffic)
			BusyThreshold = 75;
		if( pmlmepriv->LinkDetectInfo.NumRxOkInPeriod > BusyThreshold ||
			pmlmepriv->LinkDetectInfo.NumTxOkInPeriod > BusyThreshold )
		{
			bBusyTraffic = _TRUE;

			if(pmlmepriv->LinkDetectInfo.NumRxOkInPeriod > BusyThreshold)
				bRxBusyTraffic = _TRUE;

			if(pmlmepriv->LinkDetectInfo.NumTxOkInPeriod > BusyThreshold)
				bTxBusyTraffic = _TRUE;
		}

		// Higher Tx/Rx data.
		if( pmlmepriv->LinkDetectInfo.NumRxOkInPeriod > 4000 ||
			pmlmepriv->LinkDetectInfo.NumTxOkInPeriod > 4000 )
		{
			bHigherBusyTraffic = _TRUE;

			// Extremely high Rx data.
			if(pmlmepriv->LinkDetectInfo.NumRxOkInPeriod > 5000)
				bHigherBusyRxTraffic = _TRUE;

			// Extremely high Tx data.
			if(pmlmepriv->LinkDetectInfo.NumTxOkInPeriod > 5000)
				bHigherBusyTxTraffic = _TRUE;
		}

#ifdef CONFIG_TRAFFIC_PROTECT
#define TX_ACTIVE_TH 2
#define RX_ACTIVE_TH 1
#define TRAFFIC_PROTECT_PERIOD_MS 4500

	if (link_detect->NumTxOkInPeriod > TX_ACTIVE_TH
		|| link_detect->NumRxUnicastOkInPeriod > RX_ACTIVE_TH) {

		LOG_LEVEL(_drv_info_, FUNC_ADPT_FMT" acqiure wake_lock for %u ms(tx:%d,rx_unicast:%d)\n",
			FUNC_ADPT_ARG(padapter),
			TRAFFIC_PROTECT_PERIOD_MS,
			link_detect->NumTxOkInPeriod,
			link_detect->NumRxUnicastOkInPeriod);

		rtw_lock_suspend_timeout(TRAFFIC_PROTECT_PERIOD_MS);
	}
#endif

#ifdef CONFIG_TDLS
#ifdef CONFIG_TDLS_AUTOSETUP
		if( ( ptdlsinfo->watchdog_count % TDLS_WATCHDOG_PERIOD ) == 0 )	//TDLS_WATCHDOG_PERIOD * 2sec, periodically sending
			issue_tdls_dis_req( padapter, NULL );
		ptdlsinfo->watchdog_count++;
#endif //CONFIG_TDLS_AUTOSETUP
#endif //CONFIG_TDLS

#ifdef CONFIG_LPS
		// check traffic for  powersaving.
		if ((padapter->registrypriv.wifi_spec == 1) &&
			(((pmlmepriv->LinkDetectInfo.NumRxMulticastOkInPeriod +
			   pmlmepriv->LinkDetectInfo.NumRxUnicastOkInPeriod +
			   pmlmepriv->LinkDetectInfo.NumTxOkInPeriod) > 8) ||
			 (pmlmepriv->LinkDetectInfo.NumRxMulticastOkInPeriod > 2) ||
			 (pmlmepriv->LinkDetectInfo.NumRxUnicastOkInPeriod > 2))
		   ) {
			bEnterPS= _FALSE;
		} else if( ((pmlmepriv->LinkDetectInfo.NumRxUnicastOkInPeriod + pmlmepriv->LinkDetectInfo.NumTxOkInPeriod) > 8 ) ||
			(pmlmepriv->LinkDetectInfo.NumRxUnicastOkInPeriod > 2) )
		{
			//DBG_871X("Tx = %d, Rx = %d \n",pmlmepriv->LinkDetectInfo.NumTxOkInPeriod,pmlmepriv->LinkDetectInfo.NumRxUnicastOkInPeriod);
			bEnterPS= _FALSE;
		}
		else
		{
			bEnterPS= _TRUE;
		}

		// LeisurePS only work in infra mode.
		if(bEnterPS)
		{
			LPS_Enter(padapter, "TRAFFIC_IDLE");
		}
		else
		{
			LPS_Leave(padapter, "TRAFFIC_BUSY");
		}
#endif
	}
	else
	{
#ifdef CONFIG_LPS
		LPS_Leave(padapter, "NON_LINKED");
#endif
	}

	pmlmepriv->LinkDetectInfo.NumRxOkInPeriod = 0;
	pmlmepriv->LinkDetectInfo.NumTxOkInPeriod = 0;
	pmlmepriv->LinkDetectInfo.NumRxUnicastOkInPeriod = 0;
	pmlmepriv->LinkDetectInfo.NumRxMulticastOkInPeriod = 0;
	pmlmepriv->LinkDetectInfo.bBusyTraffic = bBusyTraffic;
	pmlmepriv->LinkDetectInfo.bTxBusyTraffic = bTxBusyTraffic;
	pmlmepriv->LinkDetectInfo.bRxBusyTraffic = bRxBusyTraffic;
	pmlmepriv->LinkDetectInfo.bHigherBusyTraffic = bHigherBusyTraffic;
	pmlmepriv->LinkDetectInfo.bHigherBusyRxTraffic = bHigherBusyRxTraffic;
	pmlmepriv->LinkDetectInfo.bHigherBusyTxTraffic = bHigherBusyTxTraffic;

}

void dynamic_chk_wk_hdl(_adapter *padapter, u8 *pbuf, int sz);
void dynamic_chk_wk_hdl(_adapter *padapter, u8 *pbuf, int sz)
{
	struct mlme_priv *pmlmepriv;

	if ((padapter->bDriverStopped == _TRUE)||(padapter->bSurpriseRemoved== _TRUE))
		return;

	if((void*)padapter != (void*)pbuf && padapter->pbuddy_adapter == NULL)
		return;

	padapter = (_adapter *)pbuf;

	if ((padapter->bDriverStopped == _TRUE)||(padapter->bSurpriseRemoved== _TRUE))
		return;

	pmlmepriv = &(padapter->mlmepriv);

#ifdef CONFIG_ACTIVE_KEEP_ALIVE_CHECK
#ifdef CONFIG_AP_MODE
	if(check_fwstate(pmlmepriv, WIFI_AP_STATE) == _TRUE)
	{
		expire_timeout_chk(padapter);
	}
#endif
#endif //CONFIG_ACTIVE_KEEP_ALIVE_CHECK

	#ifdef DBG_CONFIG_ERROR_DETECT
	rtw_hal_sreset_xmit_status_check(padapter);
	#endif

	//if(check_fwstate(pmlmepriv, _FW_UNDER_LINKING|_FW_UNDER_SURVEY)==_FALSE)
	{
		linked_status_chk(padapter);
		traffic_status_watchdog(padapter);
	}

	rtw_hal_dm_watchdog(padapter);

	//check_hw_pbc(padapter, pdrvextra_cmd->pbuf, pdrvextra_cmd->type_size);

}

#ifdef CONFIG_LPS

void lps_ctrl_wk_hdl(_adapter *padapter, u8 lps_ctrl_type);
void lps_ctrl_wk_hdl(_adapter *padapter, u8 lps_ctrl_type)
{
	struct pwrctrl_priv *pwrpriv = &padapter->pwrctrlpriv;
	struct mlme_priv *pmlmepriv = &(padapter->mlmepriv);
	u8	mstatus;

_func_enter_;

	if((check_fwstate(pmlmepriv, WIFI_ADHOC_MASTER_STATE) == _TRUE)
		|| (check_fwstate(pmlmepriv, WIFI_ADHOC_STATE) == _TRUE))
	{
		return;
	}

	switch(lps_ctrl_type)
	{
		case LPS_CTRL_SCAN:
			//DBG_871X("LPS_CTRL_SCAN \n");
			LeaveAllPowerSaveMode(padapter);
			break;
		case LPS_CTRL_JOINBSS:
			//DBG_871X("LPS_CTRL_JOINBSS \n");
			LPS_Leave(padapter, "LPS_CTRL_JOINBSS");
			break;
		case LPS_CTRL_CONNECT:
			//DBG_871X("LPS_CTRL_CONNECT \n");
			mstatus = 1;
			// Reset LPS Setting
			padapter->pwrctrlpriv.LpsIdleCount = 0;
			rtw_hal_set_hwreg(padapter, HW_VAR_H2C_FW_JOINBSSRPT, (u8 *)(&mstatus));
			break;
		case LPS_CTRL_DISCONNECT:
			//DBG_871X("LPS_CTRL_DISCONNECT \n");
			mstatus = 0;
			LPS_Leave(padapter, "LPS_CTRL_DISCONNECT");
			rtw_hal_set_hwreg(padapter, HW_VAR_H2C_FW_JOINBSSRPT, (u8 *)(&mstatus));
			break;
		case LPS_CTRL_SPECIAL_PACKET:
			//DBG_871X("LPS_CTRL_SPECIAL_PACKET \n");
			pwrpriv->DelayLPSLastTimeStamp = rtw_get_current_time();
			LPS_Leave(padapter, "LPS_CTRL_SPECIAL_PACKET");
			break;

		default:
			break;
	}

_func_exit_;
}

u8 rtw_lps_ctrl_wk_cmd(_adapter*padapter, u8 lps_ctrl_type, u8 enqueue)
{
	struct cmd_obj	*ph2c;
	struct drvextra_cmd_parm	*pdrvextra_cmd_parm;
	struct cmd_priv	*pcmdpriv = &padapter->cmdpriv;
	//struct pwrctrl_priv *pwrctrlpriv = &padapter->pwrctrlpriv;
	u8	res = _SUCCESS;

_func_enter_;

	//if(!pwrctrlpriv->bLeisurePs)
	//	return res;

#ifdef CONFIG_CONCURRENT_MODE
	if (padapter->iface_type != IFACE_PORT0)
		return res;
#endif

	if(enqueue)
	{
		ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if(ph2c==NULL){
			res= _FAIL;
			goto exit;
		}

		pdrvextra_cmd_parm = kzalloc(sizeof(struct drvextra_cmd_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if(pdrvextra_cmd_parm==NULL){
			rtw_mfree((unsigned char *)ph2c, sizeof(struct cmd_obj));
			res= _FAIL;
			goto exit;
		}

		pdrvextra_cmd_parm->ec_id = LPS_CTRL_WK_CID;
		pdrvextra_cmd_parm->type_size = lps_ctrl_type;
		pdrvextra_cmd_parm->pbuf = NULL;

		init_h2fwcmd_w_parm_no_rsp(ph2c, pdrvextra_cmd_parm, GEN_CMD_CODE(_Set_Drv_Extra));

		res = rtw_enqueue_cmd(pcmdpriv, ph2c);
	}
	else
	{
		lps_ctrl_wk_hdl(padapter, lps_ctrl_type);
	}

exit:

_func_exit_;

	return res;

}

#endif
#ifdef CONFIG_ANTENNA_DIVERSITY

void antenna_select_wk_hdl(_adapter *padapter, u8 antenna)
{
	rtw_hal_set_hwreg(padapter, HW_VAR_ANTENNA_DIVERSITY_SELECT, (u8 *)(&antenna));
}

u8 rtw_antenna_select_cmd(_adapter*padapter, u8 antenna,u8 enqueue)
{
	struct cmd_obj		*ph2c;
	struct drvextra_cmd_parm	*pdrvextra_cmd_parm;
	struct cmd_priv	*pcmdpriv = &padapter->cmdpriv;
	u8	bSupportAntDiv = _FALSE;
	u8	res = _SUCCESS;

_func_enter_;
	rtw_hal_get_def_var(padapter, HAL_DEF_IS_SUPPORT_ANT_DIV, &(bSupportAntDiv));
	if(_FALSE == bSupportAntDiv )	return res;

	if(_TRUE == enqueue)
	{
		ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if(ph2c==NULL){
			res= _FAIL;
			goto exit;
		}

		pdrvextra_cmd_parm = kzalloc(sizeof(struct drvextra_cmd_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
		if(pdrvextra_cmd_parm==NULL){
			rtw_mfree((unsigned char *)ph2c, sizeof(struct cmd_obj));
			res= _FAIL;
			goto exit;
		}

		pdrvextra_cmd_parm->ec_id = ANT_SELECT_WK_CID;
		pdrvextra_cmd_parm->type_size = antenna;
		pdrvextra_cmd_parm->pbuf = NULL;
		init_h2fwcmd_w_parm_no_rsp(ph2c, pdrvextra_cmd_parm, GEN_CMD_CODE(_Set_Drv_Extra));

		res = rtw_enqueue_cmd(pcmdpriv, ph2c);
	}
	else{
		antenna_select_wk_hdl(padapter,antenna );
	}
exit:

_func_exit_;

	return res;

}
#endif

void power_saving_wk_hdl(_adapter *padapter, u8 *pbuf, int sz);
void power_saving_wk_hdl(_adapter *padapter, u8 *pbuf, int sz)
{
	 rtw_ps_processor(padapter);
}

//add for CONFIG_IEEE80211W, none 11w can use it
void reset_securitypriv_hdl(_adapter *padapter)
{
	 rtw_reset_securitypriv(padapter);
}

void free_assoc_resources_hdl(_adapter *padapter)
{
	 rtw_free_assoc_resources(padapter, 1);
}

#ifdef CONFIG_P2P
u8 p2p_protocol_wk_cmd(_adapter*padapter, int intCmdType )
{
	struct cmd_obj	*ph2c;
	struct drvextra_cmd_parm	*pdrvextra_cmd_parm;
	struct wifidirect_info	*pwdinfo= &(padapter->wdinfo);
	struct cmd_priv	*pcmdpriv = &padapter->cmdpriv;
	u8	res = _SUCCESS;

_func_enter_;

	if(rtw_p2p_chk_state(pwdinfo, P2P_STATE_NONE))
	{
		return res;
	}

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm = kzalloc(sizeof(struct drvextra_cmd_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(pdrvextra_cmd_parm==NULL){
		rtw_mfree((unsigned char *)ph2c, sizeof(struct cmd_obj));
		res= _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm->ec_id = P2P_PROTO_WK_CID;
	pdrvextra_cmd_parm->type_size = intCmdType;	//	As the command tppe.
	pdrvextra_cmd_parm->pbuf = NULL;		//	Must be NULL here

	init_h2fwcmd_w_parm_no_rsp(ph2c, pdrvextra_cmd_parm, GEN_CMD_CODE(_Set_Drv_Extra));

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);

exit:

_func_exit_;

	return res;

}
#endif //CONFIG_P2P

u8 rtw_ps_cmd(_adapter*padapter)
{
	struct cmd_obj		*ppscmd;
	struct drvextra_cmd_parm	*pdrvextra_cmd_parm;
	struct cmd_priv	*pcmdpriv = &padapter->cmdpriv;

	u8	res = _SUCCESS;
_func_enter_;

#ifdef CONFIG_CONCURRENT_MODE
	if (padapter->adapter_type != PRIMARY_ADAPTER)
		goto exit;
#endif

	ppscmd = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ppscmd==NULL){
		res= _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm = kzalloc(sizeof(struct drvextra_cmd_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(pdrvextra_cmd_parm==NULL){
		rtw_mfree((unsigned char *)ppscmd, sizeof(struct cmd_obj));
		res= _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm->ec_id = POWER_SAVING_CTRL_WK_CID;
	pdrvextra_cmd_parm->pbuf = NULL;
	init_h2fwcmd_w_parm_no_rsp(ppscmd, pdrvextra_cmd_parm, GEN_CMD_CODE(_Set_Drv_Extra));

	res = rtw_enqueue_cmd(pcmdpriv, ppscmd);

exit:

_func_exit_;

	return res;

}

#ifdef CONFIG_AP_MODE

static void rtw_chk_hi_queue_hdl(_adapter *padapter)
{
	struct sta_info *psta_bmc;
	struct sta_priv *pstapriv = &padapter->stapriv;
	u32 start = rtw_get_current_time();
	u8 empty = _FALSE;

	psta_bmc = rtw_get_bcmc_stainfo(padapter);
	if(!psta_bmc)
		return;

	rtw_hal_get_hwreg(padapter, HW_VAR_CHK_HI_QUEUE_EMPTY, &empty);

	while(_FALSE == empty && rtw_get_passing_time_ms(start) < rtw_get_wait_hiq_empty_ms())
	{
		rtw_msleep_os(100);
		rtw_hal_get_hwreg(padapter, HW_VAR_CHK_HI_QUEUE_EMPTY, &empty);
	}

	if(psta_bmc->sleepq_len==0)
	{
		if(empty == _SUCCESS)
		{
			bool update_tim = _FALSE;

			if (pstapriv->tim_bitmap & BIT(0))
				update_tim = _TRUE;

			pstapriv->tim_bitmap &= ~BIT(0);
			pstapriv->sta_dz_bitmap &= ~BIT(0);

			if (update_tim == _TRUE)
				_update_beacon(padapter, _TIM_IE_, NULL, _TRUE, "bmc sleepq and HIQ empty");
		}
		else //re check again
		{
			rtw_chk_hi_queue_cmd(padapter);
		}
	}
}

u8 rtw_chk_hi_queue_cmd(_adapter*padapter)
{
	struct cmd_obj	*ph2c;
	struct drvextra_cmd_parm	*pdrvextra_cmd_parm;
	struct cmd_priv	*pcmdpriv = &padapter->cmdpriv;
	u8	res = _SUCCESS;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(ph2c==NULL){
		res= _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm = kzalloc(sizeof(struct drvextra_cmd_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if(pdrvextra_cmd_parm==NULL){
		rtw_mfree((unsigned char *)ph2c, sizeof(struct cmd_obj));
		res= _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm->ec_id = CHECK_HIQ_WK_CID;
	pdrvextra_cmd_parm->type_size = 0;
	pdrvextra_cmd_parm->pbuf = NULL;

	init_h2fwcmd_w_parm_no_rsp(ph2c, pdrvextra_cmd_parm, GEN_CMD_CODE(_Set_Drv_Extra));

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);

exit:

	return res;

}
#endif

u8 rtw_c2h_wk_cmd(PADAPTER padapter, u8 *c2h_evt)
{
	struct cmd_obj *ph2c;
	struct drvextra_cmd_parm *pdrvextra_cmd_parm;
	struct cmd_priv	*pcmdpriv = &padapter->cmdpriv;
	u8	res = _SUCCESS;

	ph2c = kzalloc(sizeof(struct cmd_obj), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (ph2c == NULL) {
		res = _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm = kzalloc(sizeof(struct drvextra_cmd_parm), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL);
	if (pdrvextra_cmd_parm == NULL) {
		rtw_mfree((u8*)ph2c, sizeof(struct cmd_obj));
		res = _FAIL;
		goto exit;
	}

	pdrvextra_cmd_parm->ec_id = C2H_WK_CID;
	pdrvextra_cmd_parm->type_size = c2h_evt?16:0;
	pdrvextra_cmd_parm->pbuf = c2h_evt;

	init_h2fwcmd_w_parm_no_rsp(ph2c, pdrvextra_cmd_parm, GEN_CMD_CODE(_Set_Drv_Extra));

	res = rtw_enqueue_cmd(pcmdpriv, ph2c);

exit:

	return res;
}

s32 c2h_evt_hdl(_adapter *adapter, struct c2h_evt_hdr *c2h_evt, c2h_id_filter filter)
{
	s32 ret = _FAIL;
	u8 buf[16];

	if (!c2h_evt) {
		/* No c2h event in cmd_obj, read c2h event before handling*/
		if (c2h_evt_read(adapter, buf) == _SUCCESS) {
			c2h_evt = (struct c2h_evt_hdr *)buf;

			if (filter && filter(c2h_evt->id) == _FALSE)
				goto exit;

			ret = rtw_hal_c2h_handler(adapter, c2h_evt);
		}
	} else {

		if (filter && filter(c2h_evt->id) == _FALSE)
			goto exit;

		ret = rtw_hal_c2h_handler(adapter, c2h_evt);
	}
exit:
	return ret;
}

#ifdef CONFIG_C2H_WK
static void c2h_wk_callback(_workitem *work)
{
	struct evt_priv *evtpriv = container_of(work, struct evt_priv, c2h_wk);
	_adapter *adapter = container_of(evtpriv, _adapter, evtpriv);
	struct c2h_evt_hdr *c2h_evt;
	c2h_id_filter ccx_id_filter = rtw_hal_c2h_id_filter_ccx(adapter);

	evtpriv->c2h_wk_alive = _TRUE;

	while (!rtw_cbuf_empty(evtpriv->c2h_queue)) {
		if ((c2h_evt = (struct c2h_evt_hdr *)rtw_cbuf_pop(evtpriv->c2h_queue)) != NULL) {
			/* This C2H event is read, clear it */
			c2h_evt_clear(adapter);
		} else if ((c2h_evt = kzalloc(16), in_interrupt() ? GFP_ATOMIC : GFP_KERNEL) != NULL) {
			/* This C2H event is not read, read & clear now */
			if (c2h_evt_read(adapter, (u8*)c2h_evt) != _SUCCESS)
				continue;
		}

		/* Special pointer to trigger c2h_evt_clear only */
		if ((void *)c2h_evt == (void *)evtpriv)
			continue;

		if (!c2h_evt_exist(c2h_evt)) {
			rtw_mfree((u8*)c2h_evt, 16);
			continue;
		}

		if (ccx_id_filter(c2h_evt->id) == _TRUE) {
			/* Handle CCX report here */
			rtw_hal_c2h_handler(adapter, c2h_evt);
			rtw_mfree((u8*)c2h_evt, 16);
		} else {
			/* Enqueue into cmd_thread for others */
			rtw_c2h_wk_cmd(adapter, (u8 *)c2h_evt);
		}
	}

	evtpriv->c2h_wk_alive = _FALSE;
}
#endif

u8 rtw_drvextra_cmd_hdl(_adapter *padapter, unsigned char *pbuf)
{
	struct drvextra_cmd_parm *pdrvextra_cmd;

	if(!pbuf)
		return H2C_PARAMETERS_ERROR;

	pdrvextra_cmd = (struct drvextra_cmd_parm*)pbuf;

	switch(pdrvextra_cmd->ec_id)
	{
		case DYNAMIC_CHK_WK_CID:
			dynamic_chk_wk_hdl(padapter, pdrvextra_cmd->pbuf, pdrvextra_cmd->type_size);
			break;
		case POWER_SAVING_CTRL_WK_CID:
			power_saving_wk_hdl(padapter, pdrvextra_cmd->pbuf, pdrvextra_cmd->type_size);
			break;
#ifdef CONFIG_LPS
		case LPS_CTRL_WK_CID:
			lps_ctrl_wk_hdl(padapter, (u8)pdrvextra_cmd->type_size);
			break;
#endif
#ifdef CONFIG_ANTENNA_DIVERSITY
		case ANT_SELECT_WK_CID:
			antenna_select_wk_hdl(padapter, pdrvextra_cmd->type_size);
			break;
#endif
#ifdef CONFIG_P2P_PS
		case P2P_PS_WK_CID:
			p2p_ps_wk_hdl(padapter, pdrvextra_cmd->type_size);
			break;
#endif // CONFIG_P2P_PS
		case P2P_PROTO_WK_CID:
			//	Commented by Albert 2011/07/01
			//	I used the type_size as the type command
			p2p_protocol_wk_hdl( padapter, pdrvextra_cmd->type_size );
			break;
#ifdef CONFIG_AP_MODE
		case CHECK_HIQ_WK_CID:
			rtw_chk_hi_queue_hdl(padapter);
			break;
#endif //CONFIG_AP_MODE
#ifdef CONFIG_INTEL_WIDI
		case INTEl_WIDI_WK_CID:
			intel_widi_wk_hdl(padapter, pdrvextra_cmd->type_size, pdrvextra_cmd->pbuf);
			break;
#endif //CONFIG_INTEL_WIDI
		//add for CONFIG_IEEE80211W, none 11w can use it
		case RESET_SECURITYPRIV:
			reset_securitypriv_hdl(padapter);
			break;
		case FREE_ASSOC_RESOURCES:
			free_assoc_resources_hdl(padapter);
			break;
		case C2H_WK_CID:
			c2h_evt_hdl(padapter, (struct c2h_evt_hdr *)pdrvextra_cmd->pbuf, NULL);
			break;

		default:
			break;
	}


	if(pdrvextra_cmd->pbuf && pdrvextra_cmd->type_size>0)
	{
		rtw_mfree(pdrvextra_cmd->pbuf, pdrvextra_cmd->type_size);
	}


	return H2C_SUCCESS;

}

void rtw_survey_cmd_callback(_adapter*	padapter ,  struct cmd_obj *pcmd)
{
	struct	mlme_priv *pmlmepriv = &padapter->mlmepriv;

_func_enter_;

	if(pcmd->res == H2C_DROPPED)
	{
		//TODO: cancel timer and do timeout handler directly...
		//need to make timeout handlerOS independent
		_set_timer(&pmlmepriv->scan_to_timer, 1);
	}
	else if (pcmd->res != H2C_SUCCESS) {
		_set_timer(&pmlmepriv->scan_to_timer, 1);
		RT_TRACE(_module_rtl871x_cmd_c_,_drv_err_,("\n ********Error: MgntActrtw_set_802_11_bssid_LIST_SCAN Fail ************\n\n."));
	}

	// free cmd
	rtw_free_cmd_obj(pcmd);

_func_exit_;
}
void rtw_disassoc_cmd_callback(_adapter*	padapter,  struct cmd_obj *pcmd)
{
	_irqL	irqL;
	struct	mlme_priv *pmlmepriv = &padapter->mlmepriv;

_func_enter_;

	if (pcmd->res != H2C_SUCCESS)
	{
		_enter_critical_bh(&pmlmepriv->lock, &irqL);
		set_fwstate(pmlmepriv, _FW_LINKED);
		_exit_critical_bh(&pmlmepriv->lock, &irqL);

		RT_TRACE(_module_rtl871x_cmd_c_,_drv_err_,("\n ***Error: disconnect_cmd_callback Fail ***\n."));

		goto exit;
	}
#ifdef CONFIG_BR_EXT
	else //clear bridge database
		nat25_db_cleanup(padapter);
#endif //CONFIG_BR_EXT

	// free cmd
	rtw_free_cmd_obj(pcmd);

exit:

_func_exit_;
}


void rtw_joinbss_cmd_callback(_adapter*	padapter,  struct cmd_obj *pcmd)
{
	struct	mlme_priv *pmlmepriv = &padapter->mlmepriv;

_func_enter_;

	if(pcmd->res == H2C_DROPPED)
	{
		//TODO: cancel timer and do timeout handler directly...
		//need to make timeout handlerOS independent
		_set_timer(&pmlmepriv->assoc_timer, 1);
	}
	else if(pcmd->res != H2C_SUCCESS)
	{
		_set_timer(&pmlmepriv->assoc_timer, 1);
	}

	rtw_free_cmd_obj(pcmd);

_func_exit_;
}

void rtw_createbss_cmd_callback(_adapter *padapter, struct cmd_obj *pcmd)
{
	_irqL irqL;
	u8 timer_cancelled;
	struct sta_info *psta = NULL;
	struct wlan_network *pwlan = NULL;
	struct	mlme_priv *pmlmepriv = &padapter->mlmepriv;
	WLAN_BSSID_EX *pnetwork = (WLAN_BSSID_EX *)pcmd->parmbuf;
	struct wlan_network *tgt_network = &(pmlmepriv->cur_network);

_func_enter_;

	if((pcmd->res != H2C_SUCCESS))
	{
		RT_TRACE(_module_rtl871x_cmd_c_,_drv_err_,("\n ********Error: rtw_createbss_cmd_callback  Fail ************\n\n."));
		_set_timer(&pmlmepriv->assoc_timer, 1 );
	}

	_cancel_timer(&pmlmepriv->assoc_timer, &timer_cancelled);

#ifdef CONFIG_FW_MLMLE
       //endian_convert
	pnetwork->Length = le32_to_cpu(pnetwork->Length);
	pnetwork->Ssid.SsidLength = le32_to_cpu(pnetwork->Ssid.SsidLength);
	pnetwork->Privacy =le32_to_cpu(pnetwork->Privacy);
	pnetwork->Rssi = le32_to_cpu(pnetwork->Rssi);
	pnetwork->NetworkTypeInUse =le32_to_cpu(pnetwork->NetworkTypeInUse);
	pnetwork->Configuration.ATIMWindow = le32_to_cpu(pnetwork->Configuration.ATIMWindow);
	//pnetwork->Configuration.BeaconPeriod = le32_to_cpu(pnetwork->Configuration.BeaconPeriod);
	pnetwork->Configuration.DSConfig =le32_to_cpu(pnetwork->Configuration.DSConfig);
	pnetwork->Configuration.FHConfig.DwellTime=le32_to_cpu(pnetwork->Configuration.FHConfig.DwellTime);
	pnetwork->Configuration.FHConfig.HopPattern=le32_to_cpu(pnetwork->Configuration.FHConfig.HopPattern);
	pnetwork->Configuration.FHConfig.HopSet=le32_to_cpu(pnetwork->Configuration.FHConfig.HopSet);
	pnetwork->Configuration.FHConfig.Length=le32_to_cpu(pnetwork->Configuration.FHConfig.Length);
	pnetwork->Configuration.Length = le32_to_cpu(pnetwork->Configuration.Length);
	pnetwork->InfrastructureMode = le32_to_cpu(pnetwork->InfrastructureMode);
	pnetwork->IELength = le32_to_cpu(pnetwork->IELength);
#endif

	_enter_critical_bh(&pmlmepriv->lock, &irqL);


	if(check_fwstate(pmlmepriv, WIFI_AP_STATE) )
	{
		psta = rtw_get_stainfo(&padapter->stapriv, pnetwork->MacAddress);
		if(!psta)
		{
		psta = rtw_alloc_stainfo(&padapter->stapriv, pnetwork->MacAddress);
		if (psta == NULL)
		{
			RT_TRACE(_module_rtl871x_cmd_c_,_drv_err_,("\nCan't alloc sta_info when createbss_cmd_callback\n"));
			goto createbss_cmd_fail ;
		}
		}

		rtw_indicate_connect( padapter);
	}
	else
	{
		_irqL	irqL;

		pwlan = _rtw_alloc_network(pmlmepriv);
		_enter_critical_bh(&(pmlmepriv->scanned_queue.lock), &irqL);
		if ( pwlan == NULL)
		{
			pwlan = rtw_get_oldest_wlan_network(&pmlmepriv->scanned_queue);
			if( pwlan == NULL)
			{
				RT_TRACE(_module_rtl871x_cmd_c_,_drv_err_,("\n Error:  can't get pwlan in rtw_joinbss_event_callback \n"));
				_exit_critical_bh(&(pmlmepriv->scanned_queue.lock), &irqL);
				goto createbss_cmd_fail;
			}
			pwlan->last_scanned = rtw_get_current_time();
		}
		else
		{
			rtw_list_insert_tail(&(pwlan->list), &pmlmepriv->scanned_queue.queue);
		}

		pnetwork->Length = get_WLAN_BSSID_EX_sz(pnetwork);
		memcpy(&(pwlan->network), pnetwork, pnetwork->Length);
		//pwlan->fixed = _TRUE;

		//rtw_list_insert_tail(&(pwlan->list), &pmlmepriv->scanned_queue.queue);

		// copy pdev_network information to	pmlmepriv->cur_network
		memcpy(&tgt_network->network, pnetwork, (get_WLAN_BSSID_EX_sz(pnetwork)));

		// reset DSConfig
		//tgt_network->network.Configuration.DSConfig = (u32)rtw_ch2freq(pnetwork->Configuration.DSConfig);

		_clr_fwstate_(pmlmepriv, _FW_UNDER_LINKING);

#if 0
		if((pmlmepriv->fw_state) & WIFI_AP_STATE)
		{
			psta = rtw_alloc_stainfo(&padapter->stapriv, pnetwork->MacAddress);

			if (psta == NULL) { // for AP Mode & Adhoc Master Mode
				RT_TRACE(_module_rtl871x_cmd_c_,_drv_err_,("\nCan't alloc sta_info when createbss_cmd_callback\n"));
				goto createbss_cmd_fail ;
			}

			rtw_indicate_connect( padapter);
		}
		else {

			//rtw_indicate_disconnect(dev);
		}
#endif
		_exit_critical_bh(&(pmlmepriv->scanned_queue.lock), &irqL);
		// we will set _FW_LINKED when there is one more sat to join us (rtw_stassoc_event_callback)

	}

createbss_cmd_fail:

	_exit_critical_bh(&pmlmepriv->lock, &irqL);

	rtw_free_cmd_obj(pcmd);

_func_exit_;

}



void rtw_setstaKey_cmdrsp_callback(_adapter*	padapter ,  struct cmd_obj *pcmd)
{

	struct sta_priv * pstapriv = &padapter->stapriv;
	struct set_stakey_rsp* psetstakey_rsp = (struct set_stakey_rsp*) (pcmd->rsp);
	struct sta_info*	psta = rtw_get_stainfo(pstapriv, psetstakey_rsp->addr);

_func_enter_;

	if(psta==NULL)
	{
		RT_TRACE(_module_rtl871x_cmd_c_,_drv_err_,("\nERROR: rtw_setstaKey_cmdrsp_callback => can't get sta_info \n\n"));
		goto exit;
	}

	//psta->aid = psta->mac_id = psetstakey_rsp->keyid; //CAM_ID(CAM_ENTRY)

exit:

	rtw_free_cmd_obj(pcmd);

_func_exit_;

}
void rtw_setassocsta_cmdrsp_callback(_adapter*	padapter,  struct cmd_obj *pcmd)
{
	_irqL	irqL;
	struct sta_priv * pstapriv = &padapter->stapriv;
	struct mlme_priv	*pmlmepriv = &padapter->mlmepriv;
	struct set_assocsta_parm* passocsta_parm = (struct set_assocsta_parm*)(pcmd->parmbuf);
	struct set_assocsta_rsp* passocsta_rsp = (struct set_assocsta_rsp*) (pcmd->rsp);
	struct sta_info*	psta = rtw_get_stainfo(pstapriv, passocsta_parm->addr);

_func_enter_;

	if(psta==NULL)
	{
		RT_TRACE(_module_rtl871x_cmd_c_,_drv_err_,("\nERROR: setassocsta_cmdrsp_callbac => can't get sta_info \n\n"));
		goto exit;
	}

	psta->aid = psta->mac_id = passocsta_rsp->cam_id;

	_enter_critical_bh(&pmlmepriv->lock, &irqL);

	if ((check_fwstate(pmlmepriv, WIFI_MP_STATE) == _TRUE) && (check_fwstate(pmlmepriv, _FW_UNDER_LINKING) == _TRUE))
		_clr_fwstate_(pmlmepriv, _FW_UNDER_LINKING);

       set_fwstate(pmlmepriv, _FW_LINKED);
	_exit_critical_bh(&pmlmepriv->lock, &irqL);

exit:
	rtw_free_cmd_obj(pcmd);

_func_exit_;
}

void rtw_getrttbl_cmd_cmdrsp_callback(_adapter*	padapter,  struct cmd_obj *pcmd);
void rtw_getrttbl_cmd_cmdrsp_callback(_adapter*	padapter,  struct cmd_obj *pcmd)
{
_func_enter_;

	rtw_free_cmd_obj(pcmd);
#ifdef CONFIG_MP_INCLUDED
	padapter->mppriv.workparam.bcompleted=_TRUE;
#endif

_func_exit_;

}
