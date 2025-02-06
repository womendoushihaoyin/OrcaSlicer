//var TestData={"sequence_id":"0","command":"get_recent_projects","response":[{"path":"D:\\work\\Models\\Toy\\3d-puzzle-cube-model_files\\3d-puzzle-cube.3mf","time":"2022\/3\/24 20:33:10"},{"path":"D:\\work\\Models\\Art\\Carved Stone Vase - remeshed+drainage\\Carved Stone Vase.3mf","time":"2022\/3\/24 17:11:51"},{"path":"D:\\work\\Models\\Art\\Kity & Cat\\Cat.3mf","time":"2022\/3\/24 17:07:55"},{"path":"D:\\work\\Models\\Toy\\鐩村墤.3mf","time":"2022\/3\/24 17:06:02"},{"path":"D:\\work\\Models\\Toy\\minimalistic-dual-tone-whistle-model_files\\minimalistic-dual-tone-whistle.3mf","time":"2022\/3\/22 21:12:22"},{"path":"D:\\work\\Models\\Toy\\spiral-city-model_files\\spiral-city.3mf","time":"2022\/3\/22 18:58:37"},{"path":"D:\\work\\Models\\Toy\\impossible-dovetail-puzzle-box-model_files\\impossible-dovetail-puzzle-box.3mf","time":"2022\/3\/22 20:08:40"}]};

var m_HotModelList=null;
var deviceClickCount = 0;
var deviceClickTimer = null;

function OnInit()
{
	//-----Official-----
    TranslatePage();

	SendMsg_GetLoginInfo();
	SendMsg_GetRecentFile();
	SendMsg_GetStaffPick();
	SendMsg_GetLocalDevices();
}

//------最佳打开文件的右键菜单功能----------
var RightBtnFilePath='';

var MousePosX=0;
var MousePosY=0;
var sImages = {};
 
function Set_RecentFile_MouseRightBtn_Event()
{
	$(".FileItem").mousedown(
		function(e)
		{			
			//FilePath
			RightBtnFilePath=$(this).attr('fpath');
			
			if(e.which == 3){
				//鼠标点击了右键+$(this).attr('ff') );
				ShowRecnetFileContextMenu();
			}else if(e.which == 2){
				//鼠标点击了中键
			}else if(e.which == 1){
				//鼠标点击了左键
				OnOpenRecentFile( encodeURI(RightBtnFilePath) );
			}
		});

	$(document).bind("contextmenu",function(e){
		//在这里书写代码，构建个性右键化菜单
		return true;
	});	
	
    $(document).mousemove( function(e){
		MousePosX=e.pageX;
		MousePosY=e.pageY;
		
		let ContextMenuWidth=$('#recnet_context_menu').width();
		let ContextMenuHeight=$('#recnet_context_menu').height();
	
		let DocumentWidth=$(document).width();
		let DocumentHeight=$(document).height();
		
		//$("#DebugText").text( ContextMenuWidth+' - '+ContextMenuHeight+'<br/>'+
		//					 DocumentWidth+' - '+DocumentHeight+'<br/>'+
		//					 MousePosX+' - '+MousePosY +'<br/>' );
	} );
	

	$(document).click( function(){		
		var e = e || window.event;
        var elem = e.target || e.srcElement;
        while (elem) {
			if (elem.id && elem.id == 'recnet_context_menu') {
                    return;
			}
			elem = elem.parentNode;
		}		
		
		$("#recnet_context_menu").hide();
	} );

	
}

function SetLoginPanelVisibility(visible) {
  var leftBoard = document.getElementById("LeftBoard");
  if (visible) {
    leftBoard.style.display = "block";
  } else {
    leftBoard.style.display = "none";
  }
}

function HandleStudio( pVal )
{
	let strCmd = pVal['command'];
	
	if (strCmd == "get_recent_projects") {
    ShowRecentFileList(pVal["response"]);
  } else if (strCmd == "studio_userlogin") {
    SetLoginInfo(pVal["data"]["avatar"], pVal["data"]["name"]);
  } else if (strCmd == "local_devices_arrived") {
	showLocalDevices(pVal["data"]);
  } else if (strCmd == "studio_useroffline") {
    SetUserOffline();
  } else if (strCmd == "studio_set_mallurl") {
    SetMallUrl(pVal["data"]["url"]);
  } else if (strCmd == "studio_clickmenu") {
    let strName = pVal["data"]["menu"];

    GotoMenu(strName);
  } else if (strCmd == "network_plugin_installtip") {
    let nShow = pVal["show"] * 1;

    if (nShow == 1) {
      $("#NoPluginTip").show();
      $("#NoPluginTip").css("display", "flex");
    } else {
      $("#NoPluginTip").hide();
    }
  } else if (strCmd == "modelmall_model_advise_get") {
    //alert('hot');
    if (m_HotModelList != null) {
      let SS1 = JSON.stringify(pVal["hits"]);
      let SS2 = JSON.stringify(m_HotModelList);

      if (SS1 == SS2) return;
    }

    m_HotModelList = pVal["hits"];
    ShowStaffPick(m_HotModelList);
  } else if (data.cmd === "SetLoginPanelVisibility") {
    SetLoginPanelVisibility(data.visible);
  }
}

function GotoMenu(strMenu) {
    let MenuList = $(".BtnItem");
    let nAll = MenuList.length;
    
    for(let n = 0; n < nAll; n++) {
        let OneBtn = MenuList[n];
        
        if($(OneBtn).attr("menu") == strMenu) {
            $(".BtnItem").removeClass("BtnItemSelected");            
            $(OneBtn).addClass("BtnItemSelected");
            
            // 隐藏所有board内容
            $("div[board]").hide();
            // 显示对应board内容
            $("div[board='" + strMenu + "']").show();
        }
    }
}

function SetLoginInfo( strAvatar, strName ) 
{
	$("#Login1").hide();
	
	$("#UserName").text(strName);
	
    let OriginAvatar=$("#UserAvatarIcon").prop("src");
	if(strAvatar!=OriginAvatar)
		$("#UserAvatarIcon").prop("src",strAvatar);
	else
	{
		//alert('Avatar is Same');
	}
	
	$("#Login2").show();
	$("#Login2").css("display","flex");
}

function SetUserOffline()
{
	$("#UserAvatarIcon").prop("src","img/c.jpg");
	$("#UserName").text('');
	$("#Login2").hide();	
	
	$("#Login1").show();
	$("#Login1").css("display","flex");
}

function SetMallUrl( strUrl )
{
	$("#MallWeb").prop("src",strUrl);
}


function ShowRecentFileList( pList )
{
	let nTotal=pList.length;
	
	let strHtml='';
	for(let n=0;n<nTotal;n++)
	{
		let OneFile=pList[n];
		
		let sPath=OneFile['path'];
		let sImg=OneFile["image"] || sImages[sPath];
		let sTime=OneFile['time'];
		let sName=OneFile['project_name'];
		sImages[sPath] = sImg;
		
		//let index=sPath.lastIndexOf('\\')>0?sPath.lastIndexOf('\\'):sPath.lastIndexOf('\/');
		//let sShortName=sPath.substring(index+1,sPath.length);
		
		let TmpHtml='<div class="FileItem"  fpath="'+sPath+'"  >'+
				'<a class="FileTip" title="'+sPath+'"></a>'+
				'<div class="FileImg" ><img src="'+sImg+'" onerror="this.onerror=null;this.src=\'img/d.png\';"  alt="No Image"  /></div>'+
				'<div class="FileName TextS1">'+sName+'</div>'+
				'<div class="FileDate">'+sTime+'</div>'+
			    '</div>';
		
		strHtml+=TmpHtml;
	}
	
	$("#FileList").html(strHtml);	
	
    Set_RecentFile_MouseRightBtn_Event();
	UpdateRecentClearBtnDisplay();
}

function ShowRecnetFileContextMenu()
{
	$("#recnet_context_menu").offset({top: 10000, left:-10000});
	$('#recnet_context_menu').show();
	
	let ContextMenuWidth=$('#recnet_context_menu').width();
	let ContextMenuHeight=$('#recnet_context_menu').height();
	
    let DocumentWidth=$(document).width();
	let DocumentHeight=$(document).height();

	let RealX=MousePosX;
	let RealY=MousePosY;
	
	if( MousePosX + ContextMenuWidth + 24 >DocumentWidth )
		RealX=DocumentWidth-ContextMenuWidth-24;
	if( MousePosY+ContextMenuHeight+24>DocumentHeight )
		RealY=DocumentHeight-ContextMenuHeight-24;
	
	$("#recnet_context_menu").offset({top: RealY, left:RealX});
}

/*-------RecentFile MX Message------*/
function SendMsg_GetLoginInfo()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="get_login_info";
	
	SendWXMessage( JSON.stringify(tSend) );	
}


function SendMsg_GetRecentFile()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="get_recent_projects";
	
	SendWXMessage( JSON.stringify(tSend) );
}

function SendMsg_GetLocalDevices() {
    var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "get_local_devices";
    
    SendWXMessage(JSON.stringify(tSend));
}

function OnLoginOrRegister()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_login_or_register";
	
	SendWXMessage( JSON.stringify(tSend) );	
}

function OnClickModelDepot()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_modeldepot";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnClickNewProject()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_newproject";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnClickOpenProject()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_openproject";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnOpenRecentFile( strPath )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_open_recentfile";
	tSend['data']={};
	tSend['data']['path']=decodeURI(strPath);
	
	SendWXMessage( JSON.stringify(tSend) );	
}

function OnDeleteRecentFile( )
{
	//Clear in UI
	$("#recnet_context_menu").hide();
	
	let AllFile=$(".FileItem");
	let nFile=AllFile.length;
	for(let p=0;p<nFile;p++)
	{
		let pp=AllFile[p].getAttribute("fpath");
		if(pp==RightBtnFilePath)
			$(AllFile[p]).remove();
	}	
	
	UpdateRecentClearBtnDisplay();
	
	//Send Msg to C++
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_delete_recentfile";
	tSend['data']={};
	tSend['data']['path']=RightBtnFilePath;
	
	SendWXMessage( JSON.stringify(tSend) );
}

function OnDeleteAllRecentFiles()
{
	$('#FileList').html('');
	UpdateRecentClearBtnDisplay();
	
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_delete_all_recentfile";
	
	SendWXMessage( JSON.stringify(tSend) );
}

function UpdateRecentClearBtnDisplay()
{
    let AllFile=$(".FileItem");
	let nFile=AllFile.length;	
	if( nFile>0 )
		$("#RecentClearAllBtn").show();
	else
		$("#RecentClearAllBtn").hide();
}




function OnExploreRecentFile( )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_explore_recentfile";
	tSend['data']={};
	tSend['data']['path']=decodeURI(RightBtnFilePath);
	
	SendWXMessage( JSON.stringify(tSend) );	
	
	$("#recnet_context_menu").hide();
}

function OnLogOut()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_logout";
	
	SendWXMessage( JSON.stringify(tSend) );	
}

function BeginDownloadNetworkPlugin()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="begin_network_plugin_download";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OutputKey(keyCode, isCtrlDown, isShiftDown, isCmdDown) {
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "get_web_shortcut";
	tSend['key_event'] = {};
	tSend['key_event']['key'] = keyCode;
	tSend['key_event']['ctrl'] = isCtrlDown;
	tSend['key_event']['shift'] = isShiftDown;
	tSend['key_event']['cmd'] = isCmdDown;

	SendWXMessage(JSON.stringify(tSend));
}

//-------------User Manual------------

function OpenWikiUrl( strUrl )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="userguide_wiki_open";
	tSend['data']={};
	tSend['data']['url']=strUrl;
	
	SendWXMessage( JSON.stringify(tSend) );	
}

//--------------Staff Pick-------
var StaffPickSwiper=null;
function InitStaffPick()
{
	if( StaffPickSwiper!=null )
	{
		StaffPickSwiper.destroy(true,true);
		StaffPickSwiper=null;
	}	
	
	StaffPickSwiper = new Swiper('#HotModel_Swiper.swiper', {
            slidesPerView : 'auto',
		    spaceBetween: 16,
			navigation: {
				nextEl: '.swiper-button-next',
				prevEl: '.swiper-button-prev',
			},
		    slidesPerView : 'auto',
		    slidesPerGroup : 3
//			autoplay: {
//				delay: 3000,
//				stopOnLastSlide: false,
//				disableOnInteraction: true,
//				disableOnInteraction: false
//			},
//			pagination: {
//				el: '.swiper-pagination',
//			},
//		    scrollbar: {
//                el: '.swiper-scrollbar',
//				draggable: true
//            }
			});
}

function SendMsg_GetStaffPick()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="modelmall_model_advise_get";
	
	SendWXMessage( JSON.stringify(tSend) );
	
	setTimeout("SendMsg_GetStaffPick()",3600*1000*1);
}

function ShowStaffPick( ModelList )
{
	let PickTotal=ModelList.length;
	if(PickTotal==0)
	{
		$('#HotModelList').html('');
		$('#HotModelArea').hide();
		
		return;
	}
	
	let strPickHtml='';
	for(let a=0;a<PickTotal;a++)
	{
		let OnePickModel=ModelList[a];
		
		let ModelID=OnePickModel['design']['id'];
		let ModelName=OnePickModel['design']['title'];
		let ModelCover=OnePickModel['design']['cover']+'?image_process=resize,w_200/format,webp';
		
		let DesignerName=OnePickModel['design']['designCreator']['name'];
		let DesignerAvatar=OnePickModel['design']['designCreator']['avatar']+'?image_process=resize,w_32/format,webp';
		
		strPickHtml+='<div class="HotModelPiece swiper-slide"  onClick="OpenOneStaffPickModel('+ModelID+')" >'+
			    '<div class="HotModel_Designer_Info"><img src="'+DesignerAvatar+'" /><span class="TextS2">'+DesignerName+'</span></div>'+
				'	<div class="HotModel_PrevBlock"><img class="HotModel_PrevImg" src="'+ModelCover+'" /></div>'+
				'	<div  class="HotModel_NameText TextS1" title="'+ModelName+'">'+ModelName+'</div>'+
				'</div>';
	}
	
	$('#HotModelList').html(strPickHtml);
	InitStaffPick();
	$('#HotModelArea').show();
}

function OpenOneStaffPickModel( ModelID )
{
	//alert(ModelID);
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="modelmall_model_open";
	tSend['data']={};
	tSend['data']['id']=ModelID;
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnAddDevice() {
    var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "homepage_add_device";
    
    SendWXMessage(JSON.stringify(tSend));
}

function OnTestBrowser(){
	var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "homepage_test_browser";
    
    SendWXMessage(JSON.stringify(tSend));
}

//---------------Global-----------------
window.postMessage = HandleStudio;

function onDeviceManagementClick(e) {
    // 阻止事件冒泡,避免触发菜单切换
    e.stopPropagation();
    
    deviceClickCount++;
    
    // 清除之前的定时器
    if (deviceClickTimer) {
        clearTimeout(deviceClickTimer);
    }
    
    // 设置新的定时器,1秒后重置点击次数
    deviceClickTimer = setTimeout(() => {
        deviceClickCount = 0;
    }, 1000);

    // 检查是否达到三次点击
    if (deviceClickCount >= 3) {
        deviceClickCount = 0;
        clearTimeout(deviceClickTimer);
        OnTestBrowser();
    }
}

function showLocalDevices(devices) {
    const deviceList = document.getElementById('DeviceList');
    // 保留第一个子元素(plus按钮),删除其他元素
    while (deviceList.children.length > 1) {
        deviceList.removeChild(deviceList.lastChild);
    }
    
    // 遍历设备列表创建卡片
    devices.forEach(device => {
        const deviceCard = document.createElement('div');
        deviceCard.className = 'DeviceCard';
        
        // 添加状态指示器
        const statusDot = document.createElement('div');
        statusDot.className = `DeviceStatus ${device.connecting ? 'connected' : 'disconnected'}`;
        deviceCard.appendChild(statusDot);
        
        // 创建图片容器
        const imgContainer = document.createElement('div');
        imgContainer.className = 'DeviceImgContainer';
        
        // 创建图片元素
        const img = document.createElement('img');
        img.className = 'DeviceImg';
        img.src = device.img || 'img/i3.png';
        img.onerror = function() {
            this.onerror = null;
            this.src = 'img/i3.png';
        };
        
        // 创建设备名称元素
        const name = document.createElement('div');
        name.className = 'DeviceName TextS1';
        name.textContent = device.dev_name || 'Unknown Device';
        
        // 添加底部操作栏
        const actions = document.createElement('div');
        actions.className = 'DeviceActions';
        
        // 重命名按钮
        const renameBtn = document.createElement('div');
        renameBtn.className = 'DeviceAction';
        renameBtn.innerHTML = '<span>重命名</span>';
        renameBtn.onclick = (e) => {
            e.stopPropagation();
            OnRenameDevice(device);
        };
        
        // 切换机型按钮
        const switchBtn = document.createElement('div');
        switchBtn.className = 'DeviceAction';
        switchBtn.innerHTML = '<span>切换机型</span>';
        switchBtn.onclick = (e) => {
            e.stopPropagation();
            OnSwitchModel(device);
        };
        
        // 连接/断开按钮
        const connectBtn = document.createElement('div');
        connectBtn.className = 'DeviceAction';
        if (device.connecting) {
            connectBtn.innerHTML = '<span>断开</span>';
            connectBtn.onclick = (e) => {
                e.stopPropagation();
                OnDisconnectDevice(device);
            };
        } else {
            connectBtn.innerHTML = '<span>连接</span>';
            connectBtn.onclick = (e) => {
                e.stopPropagation();
                OnConnectDevice(device);
            };
        }
        
        actions.appendChild(renameBtn);
        actions.appendChild(switchBtn);
        actions.appendChild(connectBtn);
        deviceCard.appendChild(actions);
        
        // 组装卡片
        imgContainer.appendChild(img);
        deviceCard.appendChild(imgContainer);
        deviceCard.appendChild(name);
        deviceList.appendChild(deviceCard);
    });
}

// 设备操作函数
function OnRenameDevice(device) {
    var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "homepage_rename_device";
    tSend['data'] = device;
    SendWXMessage(JSON.stringify(tSend));
}

function OnSwitchModel(device) {
    var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "homepage_switch_model";
    tSend['data'] = device;
    SendWXMessage(JSON.stringify(tSend));
}

function OnConnectDevice(device) {
    var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "homepage_connect_device";
    tSend['data'] = device;
    SendWXMessage(JSON.stringify(tSend));
}

function OnDisconnectDevice(device) {
    var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "homepage_disconnect_device";
    tSend['data'] = device;
    SendWXMessage(JSON.stringify(tSend));
}

